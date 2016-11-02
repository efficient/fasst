#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "hots.h"
#include "main.h"
#include <getopt.h>
#include <thread>

int main(int argc, char *argv[])
{
	static_assert(HRD_MAX_INLINE == 60, "");

	FixedTable *fixedtable[HOTS_MAX_REPLICAS] = {NULL}; _unused(fixedtable);
	Lockserver *lockserver = NULL;	/* Created only at the lockserver machine */

	auto test_config =
		::mica::util::Config::load_file("tx-test.json").get("tx_test");
	int workers_per_machine = test_config.get("workers_per_machine").get_int64();
	int num_machines = test_config.get("num_machines").get_int64();

	bool use_lock_server = test_config.get("use_lock_server").get_bool();
	int num_locks = test_config.get("num_locks_kilo").get_int64() * 1024;

	assert(workers_per_machine >= 1 && workers_per_machine <= 56);
	assert(num_machines >= 1 && num_machines <= 256);

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

	/* Initialize shared structures */
	bool am_i_lock_server = use_lock_server ?
		(machine_id == num_machines - 1) : false;
	if(am_i_lock_server) {
		/* The lockserver machine does not create tables */
		printf("main: Creating lock server\n");
		lockserver = new Lockserver(num_locks, workers_per_machine);
	} else {
		/* Create the shared FixedTable */
		int num_replicas = test_config.get("num_backups").get_int64() + 1;
		size_t val_size = (int) test_config.get("val_size").get_int64();
		assert(num_replicas >= 1 && num_replicas <= HOTS_MAX_REPLICAS);
		assert(val_size >= 1 && val_size <= HOTS_MAX_VALUE);
		
		printf("Machine %d: initializing FixedTable.\n", machine_id);

		for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
			bool is_primary = (repl_i == 0);
			fixedtable[repl_i] = ds_fixedtable_init("fixedtable.json",
				val_size, DIST_KV_BASE_SHM_KEY + repl_i, is_primary);

			/* Only initialize here. The worker threads will populate. */
		}
	}

	printf("main: Launching %d swarm workers\n", workers_per_machine);

	auto param_arr = new struct thread_params[workers_per_machine];
	auto thread_arr = new std::thread[workers_per_machine];
	auto global_stats = new global_stats_t[workers_per_machine];

	for(int i = 0; i < workers_per_machine; i++) {
		param_arr[i].wrkr_gid = (machine_id * workers_per_machine) + i;
		param_arr[i].fixedtable = fixedtable;
		param_arr[i].lockserver = lockserver;
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
