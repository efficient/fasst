#include <stdint.h>

#define USE_REQ_POSTLIST 1

#define CHECK_PACKET_LOSS 1
static_assert(CHECK_PACKET_LOSS == 1, "");

#define NUM_UD_QPS 1
#define WINDOW_SIZE 32

/*
 * Number of responses to collect before sending new requests. This is useful
 * if we want larger request postlists.
 */
#define REQ_BATCH_SIZE 32

#define NUM_WORKERS 552	/* Total workers in the swarm */
#define NUM_WORKERS_ 1023	/* The next power of 2 */

/* Maximum outstanding requests that a thread can have to any other thread */
#define PER_WORKER_CREDITS 8

#define MAX_PORTS 2

#define BUF_SIZE 4096
#define CACHELINE_SIZE 64

#define UNSIG_BATCH 64
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

#define MAX_POSTLIST 64

#define PKT_TYPE_REQ 0x0u
#define PKT_TYPE_RESP 0x1u

/*
 * Currently, we encode packet type and worker global ID in the immediate header.
 * This can be avoided later: packet type can be inferred from the size of the
 * packet bc request packets are 0-byte. The sender's can be inferred using the
 * InfiniBand LID and QPN in the wc.
 */
#define TOKEN_BITS 17
#define TOKEN_MASK ((1 << TOKEN_BITS) - 1)
#define WORKER_GID_BITS (32 - TOKEN_BITS - 1)
struct imm_payload {
	uint32_t pkt_type	:1;
	uint32_t token		:TOKEN_BITS;
	uint32_t wrkr_gid	:WORKER_GID_BITS;
};

struct thread_params {
	int id;
	double *tput;
	long long *reqs_ever;

	int num_threads;
	int base_port_index;
	int num_ports;
	int postlist;
	int size_req;
	int size_resp;
};

void *run_thread(void *arg);
