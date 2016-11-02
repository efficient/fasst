#include "main.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "hots.h"
#include <getopt.h>
#include <thread>

int main(int argc, char *argv[])
{
	assert(hrd_is_power_of_2(NUM_WORKERS_ + 1) == 1);

	int num_threads = -1, num_coro = -1;
	int machine_id = -1, postlist = -1, numa_node = -1;
	int num_keys = 0;	/* Total keys in this workers's datastore */
	int val_size = -1, put_percentage = -1;
	int base_port_index = -1, num_ports = -1, num_qps = -1;

	static struct option opts[] = {
		{"num-threads", required_argument, 0, 't' },
		{"num-coro", required_argument, 0, 'c' },
		{"base-port-index", required_argument, 0, 'b' },
		{"num-ports", required_argument, 0, 'N' },
		{"num-qps", required_argument, 0, 'q' },
		{"machine-id", required_argument, 0, 'm' },
		{"postlist", required_argument, 0, 'p' },
		{"numa-node", required_argument, 0, 'n' },
		{"num-keys-thousands", required_argument, 0, 'k' },
		{"val-size", required_argument, 0, 'v' },
		{"put-percentage", required_argument, 0, 'P' },
		{0, 0, 0, 0}
	};
		
	/* Parse and check arguments */
	int c;
	while(1) {
		c = getopt_long(argc, argv, "t:c:b:N:q:m:n:k:v:P", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 't':
				num_threads = atoi(optarg);
				break;
			case 'c':
				num_coro = atoi(optarg);
				break;
			case 'b':
				base_port_index = atoi(optarg);
				break;
			case 'N':
				num_ports = atoi(optarg);
				break;
			case 'q':
				num_qps = atoi(optarg);
				break;
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 'p':
				postlist = atoi(optarg);
				break;
			case 'n':
				numa_node = atoi(optarg);
				break;
			case 'k':
				num_keys = atoi(optarg) * 1024;
				break;
			case 'v':
				val_size = atoi(optarg);
				break;
			case 'P':
				put_percentage = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				exit(-1);
		}
	}

	/* Sanity checks */
	assert(num_threads >= 1 && num_threads <= 56 &&
		num_coro >= 2 && num_coro <= 16 &&
		base_port_index >= 0 && base_port_index <= 8 &&
		num_ports >= 1 && num_ports <= 8 &&
		num_qps >= 1 && num_qps <= 4 &&
		machine_id >= 0 && machine_id <= 256 &&
		postlist >= 1 && postlist <= 64 &&
		numa_node >= 0 && numa_node <= 3 &&
		num_keys >= 1 && num_keys <= (128 * 1024 * 1024) &&
		hrd_is_power_of_2(num_keys) &&
		val_size >= 1 && val_size <= HOTS_MAX_VALUE);

	if(USE_GET_LOCK == 1 && put_percentage != 0) {
		printf("Error. USE_GET_LOCK should (ideally..) be used without PUTs\n");
		exit(-1);
	}

	printf("main: Launching %d swarm workers\n", num_threads);
	double *tput = new double[num_threads];
	long long *reqs_ever = new long long[num_threads];
	for(int i = 0; i < num_threads; i++) {
		tput[i] = 0.0;
		reqs_ever[i] = 0;
	}

	auto param_arr = new struct thread_params[num_threads];
	auto thread_arr = new std::thread[num_threads];

	for(int i = 0; i < num_threads; i++) {
		param_arr[i].id = (machine_id * num_threads) + i;
		param_arr[i].tput = tput;
		param_arr[i].reqs_ever = reqs_ever;

		param_arr[i].num_threads = num_threads;
		param_arr[i].num_coro = num_coro;
		param_arr[i].base_port_index = base_port_index;
		param_arr[i].num_ports = num_ports;
		param_arr[i].num_qps = num_qps;
		param_arr[i].postlist = postlist;
		param_arr[i].numa_node = numa_node;
		param_arr[i].num_keys = num_keys;
		param_arr[i].val_size = val_size;
		param_arr[i].put_percentage = put_percentage;
		
		thread_arr[i] = std::thread(run_thread, &param_arr[i]);

		/* Pin thread i to hardware thread 2 * i */
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(2 * i, &cpuset);
		int rc = pthread_setaffinity_np(thread_arr[i].native_handle(),
			sizeof(cpu_set_t), &cpuset);
		if (rc != 0) {
			std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
		}
	}

	for(int i = 0; i < num_threads; i++) {
		printf("main: waiting for thread %d\n", i);
		thread_arr[i].join();
		printf("main: thread %d done\n", i);
	}

	return 0;
}
