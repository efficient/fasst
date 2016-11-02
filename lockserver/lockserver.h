#ifndef LOCKSERVER_H
#define LOCKSERVER_H

#include <stdio.h>
#include <pthread.h>

#include "hots.h"
#include "rpc/rpc.h"
#include "tx/tx_defs.h"
#include "libhrd/hrd.h"

#define LS_DEBUG_PRINTF 0

#define ls_dprintf(fmt, ...) \
	do { \
		if (LS_DEBUG_PRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)

#define ls_dassert(x) \
	do { if (LS_DEBUG_ASSERT) assert(x); } while (0)

// Lock server definitions
enum class locksrv_reqtype_t : uint16_t {
	lock = 3,
	unlock = 4,
};

enum class locksrv_resptype_t : uint16_t {
	success = 3,
	fail = 4,
};


#define locksrv_hashfrag_bits 31
#define locksrv_hashfrag_mask ((1u << locksrv_hashfrag_bits) - 1)
struct locksrv_req_t {
	/* 4 bytes: ID of the requesting coroutine */
	hots_glbl_crid_t requester_id;

	/* 4 bytes: Metadata for keys to be locked */
	locksrv_reqtype_t locksrv_reqtype;
	uint16_t num_keys;

	/* 4 bytes per key to be locked */
	struct {
		uint32_t exclusive : 32 - locksrv_hashfrag_bits;
		uint32_t hashfrag : locksrv_hashfrag_bits;
	} key_arr[RPC_MAX_MSG_CORO];
};

#define locksrv_req_size(num_keys) (sizeof(locksrv_req_t) - \
	(RPC_MAX_MSG_CORO * sizeof(uint32_t)) + ((num_keys) * sizeof(uint32_t)))

#define locksrv_lockmode_unlocked 1
#define locksrv_lockmode_exclusive 2
#define locksrv_lockmode_shared 3

#define locksrv_max_counter 512	/* Reject more sharing after this limit */

/* XXX: This is too large. Should be possible to make it 64 bits */
struct locksrv_lock_t {
	pthread_spinlock_t spinlock;	/* Held for a very short time */
	uint32_t lock_mode;
	uint32_t counter;	/* Number of sharers */

	hots_glbl_crid_t exclusive_owner;
};
static_assert(sizeof(locksrv_lock_t) == 2 * sizeof(uint64_t), "");

/*
 * The lockserver object is shared between all lockserver threads, so no stats
 * to avoid contention.
 */
class Lockserver {
private:
	int num_locks;
	int workers_per_machine;	/* Debug-only */

public:
	/* Derived values */
	int num_locks_mask;
	locksrv_lock_t *lock_arr;

	Lockserver(int num_locks, int workers_per_machine) :
		num_locks(num_locks), workers_per_machine(workers_per_machine)
	{
		ct_assert(sizeof(locksrv_req_t) ==
			sizeof(uint64_t) + RPC_MAX_MSG_CORO * sizeof(uint32_t));

		assert(hrd_is_power_of_2(num_locks));
		num_locks_mask = num_locks - 1;

		/* Initialize the lock array */
		size_t reqd_size = num_locks * sizeof(locksrv_lock_t);
		while(reqd_size % M_2 != 0) {
			reqd_size++;
		}		
		lock_arr = (locksrv_lock_t  *) hrd_malloc_socket(LOCKSERVER_SHM_KEY,
			reqd_size, 0);
		assert(lock_arr != NULL);

		for(int i = 0; i < num_locks; i++) {
			int ret = pthread_spin_init(&lock_arr[i].spinlock, 0); _unused(ret);
			assert(ret == 0);
			lock_arr[i].lock_mode = locksrv_lockmode_unlocked;
			lock_arr[i].counter = 0;
		}
	}

	forceinline bool lock(hots_glbl_crid_t requester_id,
		uint32_t exclusive, uint32_t hashfrag)
	{
		int lock_i = hashfrag & num_locks_mask;
		ls_dassert(lock_i >= 0 && lock_i < num_locks);

		locksrv_lock_t *lock = &lock_arr[lock_i];	

		int ret = pthread_spin_lock(&lock->spinlock); _unused(ret);
		ls_dassert(ret == 0);

		/* Spinlock protection begin */
		uint32_t cur_lock_mode = lock->lock_mode;
		uint32_t cur_counter = lock->counter; _unused(cur_counter);
		hots_glbl_crid_t cur_exclusive_owner = lock->exclusive_owner;

		bool success = true;

		if(exclusive == 1) {
			// Handle exclusive lock request
			switch(cur_lock_mode) {
			case locksrv_lockmode_unlocked:
				ls_dassert(cur_counter == 0);
				lock->lock_mode = locksrv_lockmode_exclusive;
				lock->counter++;
				lock->exclusive_owner = requester_id;
				ls_dprintf("Coro {%u, %u} acquiring EXCLUSIVE lock %d\n",
					requester_id.wrkr_gid, requester_id.coro_id, lock_i);
				break;

			case locksrv_lockmode_shared:
				/*
				 * In this case, this requester cannot be holding the shared
				 * lock because we grab exclusive locks first.
				 */
				ls_dassert(cur_counter > 0);
				success = false;
				break;

			case locksrv_lockmode_exclusive:
				ls_dassert(cur_counter > 0);
				if(cur_exclusive_owner == requester_id) {
					ls_dprintf("Coro {%u, %u} re-acquiring EXCLUSIVE lock %d "
						"that it already holds EXCLUSIVE-ly\n",
						requester_id.wrkr_gid, requester_id.coro_id, lock_i);
					lock->counter++;
				} else {
					success = false;
				}
				break;

			default:
				assert(false);
				exit(-1);
			} /* End switch */
		} else {
			// Handle shared lock request
			switch(cur_lock_mode) {
			case locksrv_lockmode_unlocked:
				ls_dassert(cur_counter == 0);
				lock->lock_mode = locksrv_lockmode_shared;
				ls_dprintf("Coro {%u, %u} acquiring SHARED lock %d\n",
					requester_id.wrkr_gid, requester_id.coro_id, lock_i);
				lock->counter++;
				break;

			case locksrv_lockmode_shared:
				/*
				 * In this case, this requester cannot be holding the shared
				 * lock because we grab exclusive locks first.
				 */
				ls_dassert(cur_counter > 0);
				lock->counter++;
				ls_dprintf("Coro {%u, %u} acquiring SHARED lock %d\n",
					requester_id.wrkr_gid, requester_id.coro_id, lock_i);
				/* Die for now. Ideally we want to reject the locking request. */
				ls_dassert(lock->counter <= locksrv_max_counter);
				break;

			case locksrv_lockmode_exclusive:
				ls_dassert(cur_counter > 0);
				if(cur_exclusive_owner == requester_id) {
					ls_dprintf("Coro {%u, %u} re-acquiring SHARED lock %d "
						"that it already holds EXCLUSIVE-ly\n",
						requester_id.wrkr_gid, requester_id.coro_id, lock_i);
					lock->counter++;
				} else {
					success = false;
				}
				break;

			default:
				assert(false);
				exit(-1);
			} /* End switch */
		}

		pthread_spin_unlock(&lock->spinlock);
		return success;
	}

	forceinline void decrement_and_unlock_if_zero(locksrv_lock_t *lock)
	{
		lock->counter--;
		if(lock->counter == 0) {
			lock->lock_mode = locksrv_lockmode_unlocked;
		}
	}

	forceinline void unlock(hots_glbl_crid_t requester_id,
		uint32_t exclusive, uint32_t hashfrag)
	{
		int lock_i = hashfrag & num_locks_mask;
		ls_dassert(lock_i >= 0 && lock_i < num_locks);

		locksrv_lock_t *lock = &lock_arr[lock_i];	

		int ret = pthread_spin_lock(&lock->spinlock); _unused(ret);
		ls_dassert(ret == 0);

		/* Spinlock protection begin */
		uint32_t cur_lock_mode = lock->lock_mode;
		ls_dassert(cur_lock_mode != locksrv_lockmode_unlocked);
		uint32_t cur_counter = lock->counter; _unused(cur_counter);
		hots_glbl_crid_t cur_exclusive_owner = lock->exclusive_owner;
		_unused(cur_exclusive_owner);

		if(exclusive == 1) {
			ls_dprintf("Coro {%u, %u} unlocking EXCLUSIVE lock %d\n",
				requester_id.wrkr_gid, requester_id.coro_id, lock_i);
			ls_dassert(cur_lock_mode == locksrv_lockmode_exclusive);
			ls_dassert(cur_counter > 0);
			ls_dassert(cur_exclusive_owner == requester_id);
			decrement_and_unlock_if_zero(lock);
		} else {
			// Handler shared unlock request
			switch(cur_lock_mode) {
			case locksrv_lockmode_shared:
				ls_dassert(cur_counter > 0);
				ls_dprintf("Coro {%u, %u} unlocking SHARED lock %d\n",
					requester_id.wrkr_gid, requester_id.coro_id, lock_i);
				decrement_and_unlock_if_zero(lock);
				break;

			case locksrv_lockmode_exclusive:
				ls_dprintf("Coro {%u, %u} unlocking SHARED lock %d "
					"(that it holds in EXCLUSIVE mode)\n",
					requester_id.wrkr_gid, requester_id.coro_id, lock_i);
				ls_dassert(cur_counter > 0);
				ls_dassert(cur_exclusive_owner == requester_id);
				decrement_and_unlock_if_zero(lock);
				break;

			default:
				assert(false);
				exit(-1);
			} /* End switch */
		}

		pthread_spin_unlock(&lock->spinlock);
	}

	// Accessors
	int get_num_locks_mask()
	{
		return num_locks_mask;
	}
};

forceinline size_t lockserver_rpc_handler(
	uint8_t *resp_buf, rpc_resptype_t *resp_type,
	const uint8_t *req_buf, size_t req_len, void *_lockserver)
{
	Lockserver *lockserver = static_cast<Lockserver *>(_lockserver);

	locksrv_req_t *ls_req = (locksrv_req_t *) req_buf;
	locksrv_reqtype_t req_type = ls_req->locksrv_reqtype;
	int num_keys = ls_req->num_keys;

	/* Sanity checks */
	ls_dassert(req_type == locksrv_reqtype_t::lock ||
		req_type == locksrv_reqtype_t::unlock);
	ls_dassert(num_keys <= RPC_MAX_MSG_CORO);
	ls_dassert(req_len == locksrv_req_size(ls_req->num_keys));

	for(int i = 0; i < num_keys; i++) {
		int lock_i = ls_req->key_arr[i].hashfrag & lockserver->num_locks_mask;
		__builtin_prefetch(&lockserver->lock_arr[lock_i], 1, 3);
	}

	if(req_type == locksrv_reqtype_t::lock) {
		// Handle a lock request
		for(int i = 0; i < num_keys; i++) {
			bool ret = lockserver->lock(ls_req->requester_id,
				ls_req->key_arr[i].exclusive, ls_req->key_arr[i].hashfrag);
			if(!ret) {
				/* Unlock key i - 1 --> 0 (read set first) */
				for(int j = i - 1; j >= 0; j--) {
					lockserver->unlock(ls_req->requester_id,
						ls_req->key_arr[j].exclusive,
						ls_req->key_arr[j].hashfrag);
				}

				*resp_type = (uint16_t) locksrv_resptype_t::fail;
				return 0;
			}
		}

		/* If we are here, we successfully locked all keys */
		*resp_type = (uint16_t) locksrv_resptype_t::success;
		return 0;
	} else {
		// Handle an unlock request. Unlock read set first.
		for(int i = num_keys - 1; i >= 0; i--) {
			lockserver->unlock(ls_req->requester_id,
				ls_req->key_arr[i].exclusive, ls_req->key_arr[i].hashfrag);
		}

		*resp_type = (uint16_t) locksrv_resptype_t::success;
		return 0;
	}
}

#endif
