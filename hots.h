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

/* Macros */
#define bit_capacity(b) (1 << b)

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
#define HOTS_CORO_ID_BITS 5	/* Max 32 coroutines per worker */

#define HOTS_MAX_BACKUPS 2	/* Maximum allowed value of f (fault tolerance) */
#define HOTS_MAX_REPLICAS (1 + HOTS_MAX_BACKUPS) /* 1 + f */

#define HOTS_MAX_MACHINES 126		/* Machine ID 127 can be used as invalid */
#define HOTS_MCHN_ID_BITS 7
static_assert(bit_capacity(HOTS_MCHN_ID_BITS) >= HOTS_MAX_MACHINES, "");

#define HOTS_MAX_WORKERS 8192	/* Maximum worker threads in cluster */
#define HOTS_WRKR_GID_BITS 13
static_assert(bit_capacity(HOTS_WRKR_GID_BITS) >= HOTS_MAX_WORKERS, "");

#define HOTS_MAX_SERVER_THREADS 56	/* Maximum threads per server */

#define HOTS_MAX_PORTS 2	/* Max RDMA ports used at any server */
#define HOTS_GRH_BYTES 40

// SHM keys: Exclusive range for each subsystem. Applications use keys >= 1000
/* RPC: 1 key per worker thread */
#define RPC_BASE_SHM_KEY 1
#define RPC_MAX_SHM_KEY 100

#define LOGGER_BASE_SHM_KEY 101
#define LOGGER_MAX_SHM_KEY 200

/* For lockserver, we allocate one hugepage buffer per machine */
#define LOCKSERVER_SHM_KEY 201


// Datastores
#define HOTS_MAX_VALUE 40	/* Max obj val_size. Doesn't affect performance. */

/*
 * Unordered HoTS datastores map 8-byte keys to application-level opaque value
 * buffers.
 * We do not care about MICA's internal alignment of objects - we pass 8-byte
 * aligned buffers to MICA's get() function, and MICA copies the object.
 */
typedef uint64_t hots_key_t;

// 3-bit number equal to kTimestampCanary (mica/table/fixedtable.h)
#define HOTS_VERSION_CANARY 5 /* Some magic number <= 7 */

/* Object header: the "locked" bit must be the first bit. */
struct hots_hdr_t {
	uint64_t locked :1;	/* Least significant bit! */
	uint64_t version :60;
	uint64_t canary :3;	/* Debug-only; can use these bits for version */
};

static_assert(sizeof(hots_hdr_t) == sizeof(uint64_t), "");

struct hots_obj_t {
	// Not stored as value in data stores - filled in by datastore handlers
	size_t val_size; /* Size of the application-level opaque @val buffer */
	hots_hdr_t hdr;

	// Stored as value in data stores
	uint8_t val[HOTS_MAX_VALUE];

	/* Return a string representation of object metadata (not value) */
	std::string to_string(uint64_t key)
	{
		std::string ret;
		ret += "{Key: " + std::to_string(key) + ", ";
		ret += "Hdr: (" + std::to_string(hdr.version) + " = " +
			std::to_string(hdr.locked) + "), ";
		ret += "Sz: " + std::to_string(val_size) + ", ";
		return ret;
	}
};
static_assert(sizeof(hots_obj_t) ==
	sizeof(size_t) + sizeof(hots_hdr_t) + HOTS_MAX_VALUE, "");

/* Size of a HoTS object with value size = val_size */
#define hots_obj_size(val_size) (sizeof(hots_obj_t) - \
	HOTS_MAX_VALUE + (val_size))

/* Initialize an object's header for initial population or PUTs */
static inline void hots_format_real_obj(hots_obj_t &obj, size_t val_size)
{
	/* XXX - We don't need to set the header - datastores do it internally */
	obj.val_size = val_size;
}

// Coroutines
typedef symmetric_coroutine<void>::call_type coro_call_t;
typedef symmetric_coroutine<void>::yield_type coro_yield_t;
typedef int coro_id_t;

/*
 * Global ID of a requesting coroutine. Used in the lockserver for recording
 * the exclusive owner of a lock. To keep the locks small, this structure needs
 * to use bitfields.
 *
 * Also used for saving per-global-coroutine state.
 */
struct hots_glbl_crid_t {
	union {
		struct {
			uint32_t coro_id : HOTS_CORO_ID_BITS;
			uint32_t wrkr_gid : 32 - HOTS_CORO_ID_BITS;
		};
		static_assert(32 - HOTS_CORO_ID_BITS >= HOTS_WRKR_GID_BITS, "");

		/*
		 * For comparison only. Do not use this as a dense global ID for
		 * coroutines because the number of per-worker coroutines may be
		 * smaller than (1 << HOTS_CORO_ID_BITS).
		 */
		uint32_t int_rep;
	};

	bool operator == (const hots_glbl_crid_t &r2) {
		return (int_rep == r2.int_rep);
	}
};
static_assert(sizeof(hots_glbl_crid_t) == sizeof(uint32_t), "");

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
		/*
		 * We allocate 8B-aligned @alloc_buf, but during run time, @cur_buf is
		 * only guaranteed 4B-alignment.
		 */
		return (alloc_buf != NULL && alloc_len > 0 &&
			cur_buf != NULL && cur_buf >= alloc_buf &&
			((uint64_t) alloc_buf) % sizeof(uint64_t) == 0 &&
			((uint64_t) cur_buf) % sizeof(uint32_t) == 0 &&
			length() <= alloc_len);

		return true;	/* XXX: Why do I need this to compile? */
	}

	inline size_t available_bytes()
	{
		return (alloc_len - length());
	}
};

#endif /* HOTS_H */
