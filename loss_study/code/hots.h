#ifndef HOTS_H
#define HOTS_H

#include<assert.h>
#include<malloc.h>
#include<iostream>
#include<boost/bind.hpp>
#include<boost/coroutine/all.hpp>

#include "modded_drivers.h"

using namespace std;
using namespace boost::coroutines;

// Debugging for different subsystems
#define RPC_DEBUG_ASSERT 0	/* rpc_defs.h */
#define DS_DEBUG_ASSERT 0	/* ds.h */
#define TX_DEBUG_ASSERT 0	/* tx_defs.h */
#define LS_DEBUG_ASSERT 0	/* lockserver.h */

#define RPC_DEBUG_PRINTF 0	/* rpc_defs.h */

#define HOTS_ASSERT_MSG(condition, message) \
	do { \
		if (! (condition)) { \
			std::cerr << "Assertion `" #condition "` failed in " << __FILE__ \
				<< " line " << __LINE__ << ": " << message << std::endl; \
			assert(false); \
		} \
	} while (false)


// High-level limits. Just increase for more.
#define HOTS_MCHN_ID_BITS 7	/* Max 128 machines in cluster */
#define HOTS_CORO_ID_BITS 4	/* Max 16 coroutines per worker */

#define HOTS_MAX_BACKUPS 2	/* Maximum allowed value of f (fault tolerance) */
#define HOTS_MAX_REPLICAS (1 + HOTS_MAX_BACKUPS) /* 1 + f */

#define HOTS_MAX_MACHINES 126		/* Machine ID 127 can be used as invalid */
#define HOTS_MAX_SERVER_THREADS 56	/* Maximum threads per server */
#define HOTS_MAX_WORKERS 512	/* Maximum worker threads in cluster */

#define HOTS_MAX_PORTS 2	/* Max RDMA ports used at any server */
#define HOTS_GRH_BYTES 40

// SHM keys: Exclusive range for each subsystem. Applications use keys >= 1000
/* RPC: 1 key per worker thread */
#define RPC_BASE_SHM_KEY 1
#define RPC_MAX_SHM_KEY 100

/* For logger and lockserver, we allocate one hugepage region per machine */
#define LOGGER_SHM_KEY 101
#define LOCKSERVER_SHM_KEY 102


// Datastores
#define HOTS_MAX_VALUE 40	/* Max obj val_size. Doesn't affect performance. */

/*
 * HoTS datastores (both ordered an unordered) map 8-byte keys to "objects" that
 * are laid out in memory as:
 * 0. The 8-byte key
 * 1. An 8-byte object header
 * 3. hdr.len bytes of "value"
 *
 * We do not care about MICA's internal alignment of objects - we pass 8-byte
 * aligned buffers to MICA's get() function, and MICA copies the object.
 */
typedef uint64_t hots_key_t;

/* Object header: The locked bit must be the first bit. */
#define HOTS_OBJHDR_CANARY 11	/* Some magic number <= 15 */
struct hots_objhdr_t {
	uint64_t locked :1;	/* Last bit! */
	uint64_t version :44;	/* Allows >16 trillion updates per object */
	uint64_t val_size :12;	/* Length of the value, not the object! */
	uint64_t canary :7;	/* Debug-only - can use this field for more bits! */
};

struct hots_obj_t {
	hots_objhdr_t hdr;
	uint8_t val[HOTS_MAX_VALUE];	/* -> 8B aligned if obj is 8B aligned */
};

static_assert(sizeof(hots_objhdr_t) == sizeof(uint64_t), "");
static_assert(sizeof(hots_obj_t) == sizeof(hots_objhdr_t) + HOTS_MAX_VALUE, "");

/* Size of a HoTS object with value size = val_size */
#define hots_obj_size(val_size) (sizeof(hots_obj_t) - \
	HOTS_MAX_VALUE + (val_size))

// Coroutines
typedef symmetric_coroutine<void>::call_type coro_call_t;
typedef symmetric_coroutine<void>::yield_type coro_yield_t;
typedef int coro_id_t;

/*
 * Global ID of a requesting coroutine. Used in the lockserver for recording
 * the exclusive owner of a lock. To keep the locks small, this structure needs
 * to use bitfields.
 *
 * Also used for saving per-global coroutine state.
 */
struct hots_glbl_crid_t {
	union {
		struct {
			uint16_t coro_id : HOTS_CORO_ID_BITS;
			uint16_t wrkr_gid : 16 - HOTS_CORO_ID_BITS;	/* ~12 = 4096 workers */
		};

		/*
		 * For comparison only. Do not use this as a dense global ID for
		 * coroutines because the number of per-workers coroutines may be
		 * smaller than (1 << HOTS_CORO_ID_BITS).
		 */
		uint16_t int_rep;
	};

	bool operator == (const hots_glbl_crid_t &r2) {
		return (int_rep == r2.int_rep);
	}
};


// Mbuf
struct hots_mbuf_t {
	uint8_t *alloc_buf;	/* Allocated buf */
	size_t alloc_len;	/* Bytes allocated */

	uint8_t *cur_buf;	/* Current pointer into alloc buf */

	/* Allocate the mbuf using standard malloc */
	inline void alloc(size_t len)
	{
		alloc_buf = (uint8_t *) memalign(8, len);
		assert(alloc_buf != NULL);
		alloc_len = len;
		cur_buf = alloc_buf;
	}

	/* Use a pre-allocated buffer for the mbuf */
	inline void alloc_with_buf(uint8_t *buf, size_t len)
	{
		alloc_buf = buf;
		assert(alloc_buf != NULL);
		alloc_len = len;
		cur_buf = alloc_buf;
	}

	inline size_t length()
	{
		return (size_t) (cur_buf - alloc_buf);
	}

	inline void reset()
	{
		cur_buf = alloc_buf;
	}

	inline bool is_valid()
	{
		return (alloc_buf != NULL && alloc_len > 0 &&
			cur_buf != NULL && cur_buf >= alloc_buf &&
			((uint64_t) alloc_buf) % sizeof(uint64_t) == 0 &&
			((uint64_t) cur_buf) % sizeof(uint64_t) == 0 &&
			length() <= alloc_len);

		return true;	/* XXX: Why do I need this to compile? */
	}

	inline size_t available_bytes()
	{
		return (alloc_len - length());
	}
};

#endif /* HOTS_H */
