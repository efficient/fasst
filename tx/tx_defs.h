#ifndef TX_DEFS_H
#define TX_DEFS_H

#include <stdint.h>
#include "rpc/rpc_defs.h"
#include "datastore/ds.h"

#define TX_DEBUG_PRINTF 0	/* Warning: prints on datapath */
#define TX_COLLECT_STATS 1
#define TX_CHECK_PACKET_LOSS 0

#define TX_ENABLE_LOCK_SERVER 0

// Debug macros
#define tx_dprintf(fmt, ...) \
	do { \
		if (TX_DEBUG_PRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)
#define tx_dassert(x) \
	do { if (TX_DEBUG_ASSERT) assert(x); } while (0)
#define tx_dassert_msg(x, msg) \
	do { if (TX_DEBUG_ASSERT) HOTS_ASSERT_MSG(x, msg); } while (0)
#define tx_stat_inc(x, y) \
	do {if (TX_COLLECT_STATS) x += y;} while (0)

/* The mode for a write set key */
enum class tx_write_mode_t {
	ignore,	/* For read set */
	update,
	insert,
	del,
};

/* Result of transaction exposed to user */
enum class tx_status_t {
	in_progress,
	must_abort,	/* Error during execution that must cause an abort */
	committed,
	aborted,
};


// Read/write set items
struct tx_rwset_item_t {
	/* User-supplied args */
	rpc_reqtype_t rpc_reqtype;	/* The key-value RPC type to invoke */
	hots_key_t key;
	hots_obj_t *obj;
	tx_write_mode_t write_mode;

	/* Derived, saved to avoid recomputation */
	uint64_t keyhash;
	int primary_mn;
	int backup_mn[HOTS_MAX_BACKUPS];


	// The version in @obj fetched for read set items gets overwritten during
	// validation. Maintain a separate copy below. The version in @obj for
	// write set items does not get overwritten, so no need for a separate copy.

	/* Read set tracking */
	bool exec_rs_exists;
	uint64_t exec_rs_version;

	/* Write set tracking */
	bool exec_ws_locked;	/* True iff we locked this key during execute */

	tx_rwset_item_t(rpc_reqtype_t rpc_reqtype, hots_key_t key, hots_obj_t *obj,
		tx_write_mode_t write_mode = tx_write_mode_t::ignore) :
		rpc_reqtype(rpc_reqtype), key(key), obj(obj), write_mode(write_mode) {

		keyhash = ds_keyhash(key);
	}
};

#endif /* TX_DEFS_H */
