#ifndef TX_H
#define TX_H

#include <stdio.h>
#include <vector>
#include <set>
#include <climits>

#include "hots.h"
#include "tx/tx_defs.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "datastore/ds.h"
#include "logger/logger.h"
#include "lockserver/lockserver.h"
#include "mappings/mappings.h"

class Tx {
private:
	// Constructor args
	coro_id_t coro_id;	/* Every slave coroutine creates a Tx object */
	Rpc *rpc;
	Mappings *mappings;
	Logger *logger; /* To get local logging info for this coroutine */
	const coro_call_t *coro_arr;	/* Coro array of this coroutine's thread */

	/*
	 * We store a pointer to the Rpc object's next_coro structure. The key
	 * invariant for correctness is that whenever this coroutine is running,
	 * coro_arr[next_coro[coro_id]] is a valid coroutine to switch to. This
	 * invariant holds even when we yield multiple times in the Tx subsystem.
	 */
	const coro_id_t *next_coro;

	uint32_t caller_id;	/* Caller ID of this coroutine */

	// Local logging info
	log_record_t *local_log_record;	/* Local log record for this coroutine */

	// Tracking info
	rpc_req_t *tx_req_arr[RPC_MAX_MSG_CORO];
	tx_status_t tx_status;
	bool lockserver_locked;	/* Have we locked at the lock server? */

	/* Read/Write sets */
	std::vector<tx_rwset_item_t> read_set;
	size_t rs_index;	/* Read set items up to rs_index - 1 have been read */

	std::vector<tx_rwset_item_t> write_set;
	size_t ws_index;	/* Write set items up to ws_index - 1 have been read */

	/*
	 * Track all keys in txn to avoid duplicates. Debug-only. There can be
	 * duplicate keys across different tables (i.e., different request types).
	 */
	std::set<std::pair<rpc_reqtype_t, hots_key_t>> key_set;

	// Stats
	long long stat_lockserver_lock_req;
	long long stat_lockserver_lock_req_success;
	long long stat_lockserver_unlock_req;

	forceinline void tx_yield(coro_yield_t &yield)
	{
		coro_id_t nc = next_coro[coro_id];	/* Next coroutine */
		tx_dassert(nc != coro_id && nc >= 0 && nc < RPC_MAX_CORO);
		yield(coro_arr[nc]);
	}

	/* tx_lockserver.h */
	forceinline bool send_lockserver_req(coro_yield_t &yield,
		locksrv_reqtype_t req_type);


public:
	/* Constructor */
	Tx(coro_id_t coro_id, Rpc *rpc, Mappings *mappings, Logger *logger,
		coro_call_t *coro_arr):
		coro_id(coro_id), rpc(rpc), mappings(mappings), logger(logger),
		coro_arr(coro_arr) {

		assert(coro_id >= 1 && coro_id < RPC_MAX_CORO);	/* No Tx for master */
		assert(rpc != NULL && mappings != NULL && logger != NULL &&
			coro_arr != NULL);

		if(mappings->use_lock_server) {
			assert(TX_ENABLE_LOCK_SERVER == 1);
		}

		/* Get coroutine info from Rpc */
		next_coro = rpc->get_next_coro_arr();
		assert(next_coro != NULL);

		/* Initialize caller ID once to avoid recomputation */
		hots_glbl_crid_t *global_coro_id = (hots_glbl_crid_t *) &caller_id;
		global_coro_id->coro_id = coro_id;
		global_coro_id->wrkr_gid = mappings->wrkr_gid;

		/* Get this coro's local log record to write directly during commit */
		local_log_record = logger->get_log_record(mappings->machine_id,
			coro_id);

		/* Initialize Tx fields */
		tx_status = tx_status_t::aborted;
		read_set.reserve(RPC_MAX_MSG_CORO);
		write_set.reserve(RPC_MAX_MSG_CORO);

		lockserver_locked = false;

		reset_stats();
	}

	/* Start a new transaction */
	forceinline void start()
	{
		/* Txns must be committed or aborted before starting a new txn */
		tx_dassert(tx_status == tx_status_t::committed ||
			tx_status == tx_status_t::aborted);

		lockserver_locked = false;
		tx_status = tx_status_t::in_progress;

		read_set.clear();
		rs_index = 0;

		write_set.clear();
		ws_index = 0;

#if TX_DEBUG_ASSERT == 1
		key_set.clear();
#endif
	}

	/*
 	 * Add a read-only key. When this key is read, the fetched object will be
	 * copied to obj.
	 * Returns the primary machine number for the key. This information is
	 * useful to avoid RPC coalescing in benchmarks.
	 */
	forceinline int add_to_read_set(rpc_reqtype_t rpc_reqtype,
		hots_key_t key, hots_obj_t *obj)
	{
		tx_dassert(tx_status == tx_status_t::in_progress);
		tx_dassert(obj != NULL);

		/* rpc_reqtype should correspond to a primary store */
		tx_dassert(rpc_reqtype % RPC_PRIMARY_DS_REQ_SPACING == 0);

#if TX_DEBUG_ASSERT == 1
		/* Ensure unique keys in transaction key set */
		if(key_set.count(std::make_pair(rpc_reqtype, key)) == 1) {
			fprintf(stderr, "Tx: Error. Key %lu already exists in transaction\n",
				key);
			exit(-1);
		}
		key_set.insert(std::make_pair(rpc_reqtype, key));
#endif
		tx_rwset_item_t item(rpc_reqtype, key, obj);
		item.primary_mn = mappings->get_primary_mn(item.keyhash);
		for(int i = 0; i < mappings->num_backups; i++) {
			item.backup_mn[i] =
				mappings->get_backup_mn_from_primary(item.primary_mn, i);
		}

		read_set.push_back(item);
		return item.primary_mn;
	}

	/*
 	 * Add a read-write key. When this key is read, the fetched object will be
	 * copied to obj.
	 * Returns the primary machine number for the key. This information is
	 * useful to avoid RPC coalescing in benchmarks.
	 */
	forceinline int add_to_write_set(rpc_reqtype_t rpc_reqtype,
		hots_key_t key, hots_obj_t *obj, tx_write_mode_t write_mode)
	{
		tx_dassert(tx_status == tx_status_t::in_progress);
		tx_dassert(obj != NULL);
		tx_dassert(write_mode != tx_write_mode_t::ignore);

		/* rpc_reqtype should correspond to a primary store */
		tx_dassert(rpc_reqtype % RPC_PRIMARY_DS_REQ_SPACING == 0);

#if TX_DEBUG_ASSERT == 1
		/* Ensure unique keys in transaction key set */
		if(key_set.count(std::make_pair(rpc_reqtype, key)) == 1) {
			fprintf(stderr, "Tx: Error. Key %lu already exists in transaction\n",
				key);
			exit(-1);
		}
		key_set.insert(std::make_pair(rpc_reqtype, key));
#endif
		tx_rwset_item_t item(rpc_reqtype, key, obj, write_mode);
		item.primary_mn = mappings->get_primary_mn(item.keyhash);
		for(int i = 0; i < mappings->num_backups; i++) {
			item.backup_mn[i] =
				mappings->get_backup_mn_from_primary(item.primary_mn, i);
		}

		write_set.push_back(item);
		return item.primary_mn;
	}

	// User API

	/* tx_execute.h */
	forceinline tx_status_t do_read(coro_yield_t &yield);

	/* Commit a transaction that only reads a single object */
	forceinline void commit_single_read() {
		tx_dassert(read_set.size() == 1 && write_set.size() == 0);
		tx_status = tx_status_t::committed;
	}

	/* Abort a read-only transaction */
	forceinline void abort_rdonly() {
		tx_dassert(read_set.size() >= 0 && write_set.size() == 0);
		tx_status = tx_status_t::aborted;
	}

	/*
	 * Check an item that has either been successfully read from a data store,
	 * or has been prepared for commit.
	 */
	forceinline void check_item(tx_rwset_item_t &item)
	{
		_unused(item);
		tx_dassert(item.obj != NULL);
		tx_dassert(item.obj->hdr.canary == HOTS_VERSION_CANARY);
		tx_dassert(item.obj->val_size > 0);
		tx_dassert(item.obj->val_size % sizeof(uint64_t) == 0);
	}

	/* tx_commit.h */
	forceinline void send_updates_to_replicas(coro_yield_t &yield,
		std::vector<int> replica_vec);
	forceinline tx_status_t commit(coro_yield_t &yield);
	forceinline void abort(coro_yield_t &yield);
	forceinline bool validate(coro_yield_t &yield);

	/* tx_logger.h */
	forceinline bool log(coro_yield_t &yield);

	/*
	 * Returns the maximum RPC-level packet size (inclusive of coalesced
	 * message request headers) generated by a transaction that updates
	 * @num_keys keys with value size up to @val_size.
	 */
	static size_t max_pkt_size(int num_keys, size_t val_size)
	{
		/*
		 * Both PUT requests and log requests contain 3 64-bit metadata words
		 * per value buffer. PUT requests contain caller ID/size, keyhash and
		 * key. Log requests contain key, size, and header.
		 *
		 * A coalesced PUT request packet contains a rpc_cmsg_reqhdr_t struct
		 * for each key, so it can be larger than the log record packet when
		 * there are a large number of keys.
		 */
		assert(ds_put_req_size(val_size) == 3 * sizeof(uint64_t) + val_size);

		size_t max_put_req_size = num_keys *
			(sizeof(rpc_cmsg_reqhdr_t) + ds_put_req_size(val_size));

		size_t max_log_record_size = sizeof(uint64_t) + /* Log record header */
			sizeof(rpc_cmsg_reqhdr_t) + /* One message in the batch */
			num_keys * (3 * sizeof(uint64_t) + val_size); /* Per-key data */

		if(max_put_req_size > max_log_record_size) {
			return max_put_req_size;
		} else {
			return max_log_record_size;
		}
	}

	// Stats
	std::string get_stats()
	{
#if TX_COLLECT_STATS == 1
		std::string ret = "Lockserver: lock = ";
		ret += std::to_string(stat_lockserver_lock_req_success);
		ret += "/";
		ret += std::to_string(stat_lockserver_lock_req);

		ret += ", unlock = ";
		ret += std::to_string(stat_lockserver_unlock_req);

		reset_stats();
		return ret;
#else
		std::string ret = "Tx stats disabled";
		return ret;
#endif
	}

	void reset_stats()
	{
		stat_lockserver_lock_req = 0;
		stat_lockserver_lock_req_success = 0;
		stat_lockserver_unlock_req = 0;
	}
};

#include "tx_lockserver.h"
#include "tx_logger.h"
#include "tx_execute.h"
#include "tx_commit.h"

#endif /* TX_H */
