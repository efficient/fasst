#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "main.h"
#include <getopt.h>
#include <thread>

int main(int argc, char *argv[])
{
	static_assert(HRD_MAX_INLINE == 60, "");

	auto test_config =
		::mica::util::Config::load_file("rpc-test.json").get("rpc_test");

	int workers_per_machine = test_config.get("workers_per_machine").get_int64();
	int num_machines = test_config.get("num_machines").get_int64();
	_unused(num_machines);

	int machine_id = -1;
	static struct option opts[] = {
		{"machine-id", required_argument, 0, 'm' },
		{0, 0, 0, 0}
	};
		
	/* Parse and check arguments */
	int c;
	while(1) {
		c = getopt_long(argc, argv, "m", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 'm':
				machine_id = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				exit(-1);
		}
	}

	/* Sanity checks */
	assert(machine_id >= 0 && machine_id < num_machines);

	printf("main: Launching %d swarm workers\n", workers_per_machine);
	double *tput = new double[workers_per_machine];
	long long *reqs_ever = new long long[workers_per_machine];
	for(int i = 0; i < workers_per_machine; i++) {
		tput[i] = 0.0;
		reqs_ever[i] = 0;
	}

	auto param_arr = new struct thread_params[workers_per_machine];
	auto thread_arr = new std::thread[workers_per_machine];
	auto global_stats = new global_stats_t[workers_per_machine];

	for(int i = 0; i < workers_per_machine; i++) {
		param_arr[i].wrkr_gid = (machine_id * workers_per_machine) + i;
		param_arr[i].global_stats = global_stats;
		
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

	for(int i = 0; i < workers_per_machine; i++) {
		printf("main: waiting for thread %d\n", i);
		thread_arr[i].join();
		printf("main: thread %d done\n", i);
	}

	return 0;
}
