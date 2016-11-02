#include "main.h"
#include "hrd.h"
#include <getopt.h>

int main(int argc, char *argv[])
{
	/* Static checks */
	static_assert(NUM_WORKERS < (1 << WORKER_GID_BITS), "");
	static_assert(sizeof(struct imm_payload) == 4, "");	/* RDMA immediate data */
	static_assert(MAX_POSTLIST < HRD_RQ_DEPTH, "");	/* For static sizing of *wc */

#if CHECK_PACKET_LOSS == 1
	static_assert(NUM_UD_QPS == 1, ""); /* Pkts across QPs can be re-ordered */
	/*
	 * In packet loss detection mode, do not send a new batch until the
	 * current batch completes. This allows us to detect packet loss by
	 * examining per-thread number of requests ever sent
	 * (in record_total_reqs_ever), because a thread will stop generating
	 * new requests after a packet loss.
	 */
	static_assert(WINDOW_SIZE == REQ_BATCH_SIZE, "");
#endif

	int i, c;
	int num_threads = -1;
	int machine_id = -1, size_req = -1, size_resp = -1, postlist = -1;
	int base_port_index = -1, num_ports = -1;	/* Same across all swarmhosts */
	struct thread_params *param_arr;
	pthread_t *thread_arr;

	static struct option opts[] = {
		{"num-threads", required_argument, 0, 't' },
		{"machine-id", required_argument, 0, 'm' },
		{"base-port-index", required_argument, 0, 'b' },
		{"num-ports", required_argument, 0, 'N' },
		{"size-req", required_argument, 0, 's' },
		{"size-resp", required_argument, 0, 'S' },
		{"postlist", required_argument, 0, 'p' },
		{0, 0, 0, 0}
	};

	/* Parse and check arguments */
	while(1) {
		c = getopt_long(argc, argv, "t:b:N:m:p:s:S", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 't':
				num_threads = atoi(optarg);
				break;
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 'b':
				base_port_index = atoi(optarg);
				break;
			case 'N':
				num_ports = atoi(optarg);
				break;
			case 's':
				size_req = atoi(optarg);
				break;
			case 'S':
				size_resp = atoi(optarg);
				break;
			case 'p':
				postlist = atoi(optarg);
				break;
			default:
				printf("Invalid argument %c\n", c);
				assert(false);
		}
	}

	/* Common checks */
	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(num_ports == 1 || num_ports == 2);	/* Need power-of-2 */
	assert(num_threads >= 1);
	assert(size_req >= 0 && size_req <= HRD_MAX_INLINE);
	assert(size_resp >= 0 && size_resp <= HRD_MAX_INLINE);
	assert(machine_id >= 0);

	assert(postlist >= 1 && postlist <= MAX_POSTLIST);
	assert(postlist <= UNSIG_BATCH);	/* Postlist check */
	assert(HRD_SQ_DEPTH >= 2 * UNSIG_BATCH);	/* Queue capacity check */

	printf("main: Launching %d swarm workers\n", num_threads);
	double *tput = (double *) malloc(num_threads * sizeof(double));
	long long *reqs_ever = (long long *) malloc(num_threads * sizeof(long long));
	for(i = 0; i < num_threads; i++) {
		tput[i] = 0.0;
		reqs_ever[i] = 0;
	}

	param_arr = (struct thread_params *)
		malloc(num_threads * sizeof(struct thread_params));
	thread_arr = (pthread_t *) malloc(num_threads * sizeof(pthread_t));

	for(i = 0; i < num_threads; i++) {
		param_arr[i].id = (machine_id * num_threads) + i;
		param_arr[i].tput = tput;
		param_arr[i].reqs_ever = reqs_ever;

		param_arr[i].num_threads = num_threads;
		param_arr[i].base_port_index = base_port_index;
		param_arr[i].num_ports = num_ports;
		param_arr[i].postlist = postlist;
		param_arr[i].size_req = size_req;
		param_arr[i].size_resp = size_resp;
		
		pthread_create(&thread_arr[i], NULL, run_thread, &param_arr[i]);
	}

	for(i = 0; i < num_threads; i++) {
		pthread_join(thread_arr[i], NULL);
	}

	return 0;
}
