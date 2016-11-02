#ifndef RPC_DEFS_H
#define RPC_DEFS_H

#include <stdio.h>
#include "libhrd/hrd.h"
#include "hots.h"
#include "rpc/rpc_types.h"

#define RPC_CMSG_REQ_HDR_MAGIC (11)	/* Some 4-bit number */

// Packet loss detection
#define RPC_LOSS_DETECTION_MS 1000  
#define RPC_LOSS_DETECTION_STEP 10000

// Optimizations
#define RPC_ENABLE_MODDED_DRIVER 1
#define RPC_UNSIG_BATCH 128
#define RPC_MAX_POSTLIST 64
#define RPC_ENABLE_MICA_PREFETCH 1

#if RPC_ENABLE_MODDED_DRIVER == 1
	static_assert(HRD_RQ_DEPTH == 256 || HRD_RQ_DEPTH == 512 ||
		HRD_RQ_DEPTH == 1024 || HRD_RQ_DEPTH == 2048 || HRD_RQ_DEPTH == 4096,
		"HRD_RQ_DEPTH must be power-of-two for modded driver");
#endif

// Debugging

/*
 * RPC_MSR_MAX_BATCH_LATENCY is not required for packet loss detection. It is
 * used only to get an estimate of max RPC batch latency, which is needed for
 * the actual, lower-overhead, always-enabled packet loss detection.
 */
#define RPC_MSR_MAX_BATCH_LATENCY 0
#define RPC_COLLECT_STATS 0
#define RPC_COLLECT_TIMER_INFO 0

// Debug macros
#define rpc_dprintf(fmt, ...) \
	do { \
		if (RPC_DEBUG_PRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)
#define rpc_dassert(x) \
	do { if (RPC_DEBUG_ASSERT) assert(x); } while (0)
#define rpc_dassert_msg(x, msg) \
	do { if (RPC_DEBUG_ASSERT) HOTS_ASSERT_MSG(x, msg); } while (0)
#define rpc_stat_inc(x, y) \
	do {if (RPC_COLLECT_STATS) x += y;} while (0)

#if RPC_COLLECT_TIMER_INFO == 1
#define rpc_get_cycles() hrd_get_cycles()
#else
#define rpc_get_cycles() 0
#endif

// RPC constants

/* Maximum pkt size can be 4096 bytes, but I'm paranoid about way conflicts */
#define RPC_MAX_MAX_PKT_SIZE 4032
#define RPC_MAX_QPS 3	/* Maximum QPs per port */

/* Buffer sizes */
#define RPC_RECV_BUF_SIZE M_32	/* Space for RECVs */
#define RPC_MBUF_SPACE M_32	/* Registered hugepages for req/resp mbufs */
#define RPC_NON_INLINE_SPACE M_32	/* Registered memory for non-inline resps */

#define RPC_MAX_CORO 32	/* Coroutines per RPC endpoint, including master */
#define RPC_MASTER_CORO_ID 0	/* The only coroutine allowed to send resps */
#define RPC_MAX_MSG_CORO 16	/* Max outstanding messages per slave coroutine.
							 * Needed for RECV maintenance. */

#define RPC_MIN_RECV_SLACK 32
#define RPC_INVALID_MN -1	/* An invalid worker number */

// Immediate data formatting
#define RPC_IS_REQ_BITS 1
#define RPC_NUM_REQS_BITS 5	/* Max requests in the coalesced message = 31 */
#define RPC_CONFIG_ID_BITS 4	/* XXX too low: Max 16 reconfigurations */

/* RPC_NUM_REQS_BITS is used to hold @num_reqs, which is 1-based */
static_assert(RPC_MAX_MSG_CORO <= ((1 << RPC_NUM_REQS_BITS) - 1), "");

/* XXX: We can probably create these beforehand (per config) */
/*
 * num_reqs is the number of requests in the coalesced request, which is equal
 * to the number of responses in the corresponding coalesced reply.
 */
union rpc_imm {
	struct {
		uint32_t is_req :RPC_IS_REQ_BITS;
		uint32_t num_reqs :RPC_NUM_REQS_BITS;
		uint32_t config_id :RPC_CONFIG_ID_BITS;	/* Unused for now */
		uint32_t mchn_id :HOTS_MCHN_ID_BITS; /* Source machine: for reply */
		uint32_t coro_id :HOTS_CORO_ID_BITS;
	};

	uint32_t int_rep;
};
static_assert(sizeof(union rpc_imm) == sizeof(uint32_t), ""); /* IB immediate */

// Messaging interface
typedef uint8_t rpc_reqtype_t;
typedef uint8_t rpc_resptype_t;

/* Header of a request in a coalesced message */
struct rpc_cmsg_reqhdr_t {
	union {
		rpc_reqtype_t req_type;
		rpc_resptype_t resp_type;
	};
	uint8_t coro_seqnum;
	uint8_t magic :4;
	uint16_t size :12; /* Size of request/resp (bytes) excluding header */
};
static_assert(sizeof(rpc_cmsg_reqhdr_t) == sizeof(uint32_t), "");

/* A coalesced message */
struct rpc_cmsg_t {
	int remote_mn;	/* The remote machine */
	uint32_t resp_imm;	/* Saved immediate at responder; unused at requester */

	int num_centry;	/* Number of coalesced requests (or responses) in the msg */
	hots_mbuf_t req_mbuf;	/* Buffer for coalesced requests */
	hots_mbuf_t resp_mbuf;	/* Buffer for the coalesced response */
};
static_assert(sizeof(rpc_cmsg_t) <= 64, "");	/* Just for performance */

/*
 * On creating an RPC request, the user gets an RPC-owned buffer (req->usr_buf)
 * to write a request to. On request completion, the RPC subsystem copies
 * the response to a user-owned response buffer (req->usr_resp_buf).
 *
 * Notes:
 * 1. req_buf is invalid after send_reqs() is called by an application.
 * 2. req_buf and resp_buf below are different from req_mbuf and resp_mbuf
 *    in the coalesced message structure (rpc_cmsg_t). req->req_buf is a pointer
 *    into cmsg->req_mbuf. req->resp_buf is a user-owned buffer. 
 */
struct rpc_req_t {
	/* The RPC user appends the request here: */
	uint8_t *req_buf;	/* Pointer into a coalesced message's req_mbuf */
	size_t req_len;

	/*
	 * Pointers into the coalesced message. We cannot rely on inferring the
	 * coalesced message request header of this request from only the coalesced
	 * mbuf because more requests may have been appended after this request.
	 */
	hots_mbuf_t *_cmsg_req_mbuf;	/* Bump this on freeze()ing */
	rpc_cmsg_reqhdr_t *_cmsg_reqhdr;	/* Put req_len here on freeze()ing */

	uint8_t *resp_buf;	/* User-supplied */
	size_t max_resp_len;	/* Size of the user-supplied buffer */

	size_t resp_len;	/* Actual response size */
	rpc_resptype_t resp_type;

	/* Append _req_len bytes to this request */
	inline void freeze(size_t _req_len)
	{
		rpc_dassert(_req_len % sizeof(uint64_t) == 0);
		rpc_dassert(_cmsg_reqhdr != NULL && _cmsg_req_mbuf != NULL);

		/* Fill in size field of the coalesced message's request header */
		rpc_dassert(_cmsg_reqhdr->magic == RPC_CMSG_REQ_HDR_MAGIC);
		_cmsg_reqhdr->size = _req_len;
		
		req_len = _req_len;	/* Local req_len is currently unused */
		_cmsg_req_mbuf->cur_buf += _req_len;	

		rpc_dassert(_cmsg_req_mbuf->is_valid());
	}

	/* Bytes available to an RPC user */
	inline size_t available_bytes()
	{
		return _cmsg_req_mbuf->available_bytes();
	}
};

/* One @rpc_req_batch_t structure per Rpc object */
struct rpc_req_batch_t {
	rpc_req_t req_arr[RPC_MAX_MSG_CORO];	/* Mostly for RPC users */

	int num_reqs;	/* Number of requests in currently active batch */
	int num_reqs_done;	/* Number of requests completed */

	/* Coalesced messages */
	int num_uniq_mn;	/* Equal to number of used cmsg_arr entries */
	int cmsg_for_mc[HOTS_MAX_MACHINES];
	rpc_cmsg_t cmsg_arr[RPC_MAX_MSG_CORO];

	/* Reset the used coalesced message - for runtime use */
	inline void clear()
	{
		/* Clearing should only be done after receiving all completions */
		rpc_dassert(num_reqs_done == num_reqs);
		num_reqs = 0;
		num_reqs_done = 0;

		for(int i = 0; i < num_uniq_mn; i++) {
			rpc_dassert(cmsg_for_mc[cmsg_arr[i].remote_mn] != -1);
			cmsg_for_mc[cmsg_arr[i].remote_mn] = -1;	/* Invalidate */

			cmsg_arr[i].remote_mn = RPC_INVALID_MN;
			cmsg_arr[i].num_centry = 0;
			cmsg_arr[i].req_mbuf.reset();
			cmsg_arr[i].resp_mbuf.reset();
		}

		num_uniq_mn = 0;	/* Reset after the loop above */
	}
};

/* One @rpc_resp_batch_t structure per Rpc object (for the master coro) */
struct rpc_resp_batch_t {
	int num_cresps;	/* Number of coalesced responses == used cmsg_arr entries */
	rpc_cmsg_t cmsg_arr[HRD_RQ_DEPTH];

	/* For copying large responses. Not needed for requests - see Rpc README */
	uint8_t *non_inline_bufs[HRD_RQ_DEPTH];
	int non_inline_index;

	inline void clear()
	{
		/* Reset the coalesced response mbufs */
		for(int i = 0; i < num_cresps; i++) {
			cmsg_arr[i].resp_mbuf.reset();
		}

		num_cresps = 0;
	}
};

/* RPC constructor arguments */
struct rpc_args {
	// Constructor args
	int wrkr_gid;	/* Global ID of this worker */
	int wrkr_lid;	/* Local ID of this worker. For computing SHM key only. */

	int num_workers;	/* Number of workers in the swarm */
	int workers_per_machine;	/* Number of workers per machine */
	int num_coro;	/* Number of coroutines used by this RPC endpoint */

	int base_port_index;
	int num_ports;
	int num_qps;	/* Number of QPs to use per port */
	int numa_node;	/* Socket to allocate hugepages from */
	int postlist;	/* Maximum postlist size; larger batches use > 1 send()s. */
	size_t max_pkt_size;	/* Mbuf size for holding coalesced reqs/resps */

	// Derived
	int num_machines;	/* Total machines in cluster */
	int machine_id;	/* ID of this machine */

	rpc_args(int wrkr_gid, int wrkr_lid,
		int num_workers, int workers_per_machine,
		int num_coro,
		int base_port_index, int num_ports,
		int num_qps, int numa_node, int postlist,
		int max_pkt_size) :
		wrkr_gid(wrkr_gid), wrkr_lid(wrkr_lid),
		num_workers(num_workers), workers_per_machine(workers_per_machine),
		num_coro(num_coro),
		base_port_index(base_port_index), num_ports(num_ports),
		num_qps(num_qps), numa_node(numa_node), postlist(postlist),
		max_pkt_size(max_pkt_size) {

		assert(num_workers > workers_per_machine);
		assert(num_workers % workers_per_machine == 0);

		// Compute derived parameters
		num_machines = num_workers / workers_per_machine;
		machine_id = wrkr_gid / workers_per_machine;
	}

	/* Default constructor; never used */
	rpc_args() : wrkr_gid(-1), num_qps(-1) {
	}
};

#endif /* RPC_DEFS */
