#include <stdint.h>
#include <stdlib.h>

#define REQ_BATCH_SIZE 16

#define NUM_WORKERS 84	/* Total workers in the swarm */
#define NUM_WORKERS_ 127	/* The next power of 2 */

#define CHECK_RESP 0	/* Check the value and print stats */

/* Prevent coalescing by sending all requests in a batch to different workers */
#define USE_UNIQUE_WORKERS 1

/* Avoid 2 active QPs/thread by sending all reqs to workers on the same port */
#define USE_SAME_PORT_WORKERS 0

/* Use get_lock() requests instead of get() */
#define USE_GET_LOCK 0

#define DIST_KV_BASE_SHM_KEY 1200
#define DIST_KV_MAX_SHM_KEY 1400

/*
 * Initially, the value for all keys in the datastore is set to INITIAL_VALUE.
 * In a PUT operation, a worker writes value = its wrkr_gid.
 */
#define INITIAL_VALUE 255

struct thread_params {
	int id;
	double *tput;
	long long *reqs_ever;

	int num_threads;
	int num_coro;
	int base_port_index;
	int num_ports;
	int num_qps;
	int postlist;
	int numa_node;
	int num_keys;
	size_t val_size;
	int put_percentage;
};

void run_thread(struct thread_params *params);
