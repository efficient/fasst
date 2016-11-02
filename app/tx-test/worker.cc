#include <boost/coroutine/all.hpp>
#include <boost/bind.hpp>
#include <papi.h>

#include "hots.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "mappings/mappings.h"
#include "tx/tx.h"
#include "datastore/fixedtable/ds_fixedtable.h"
#include "util/rte_memcpy.h"
#include "mica/util/zipf.h"
#include "mica/util/tsc.h"

#include "main.h"

#define USE_ZIPF 0	/* If 0, uniform random dist based on fastrand is used */
#define MEASURE_LATENCY 0	/* Should we measure transaction latency? */

/* Use the pre-emptable lockserver that directly uses Rpc's queue pairs */
#define USE_RPC_LAYER_LOCKSERVER 0

__thread int wrkr_gid;	/* Global ID of this worker */
__thread int wrkr_lid;	/* Local ID of this worker */
__thread int num_coro, base_port_index, num_ports, num_qps, postlist, numa_node;
__thread int num_machines, workers_per_machine, num_workers;
__thread int num_backups;
__thread bool use_lock_server;

/* Application parameters */
__thread int num_keys_kilo;
__thread size_t num_keys_global, val_size;
__thread double zipf_theta;
__thread int read_set_size, write_percentage;
__thread global_stats_t *global_stats;

/* High-level HoTS structures */
__thread coro_call_t *coro_arr;
__thread coro_id_t *next_coro;
__thread Rpc *rpc;
__thread Logger *logger;
__thread Mappings *mappings;
__thread Lockserver *lockserver;

/* Stats */
__thread size_t stat_tx_tot = 0;
__thread double stat_tx_tot_usec = 0;
__thread size_t stat_ex_fail = 0; /* Transactions that failed execution */
__thread size_t stat_commit_fail = 0; /* Transactions that failed commit */
__thread size_t stat_commit_success = 0; /* Transactions that committed */

__thread uint64_t tg_seed = 0xdeadbeef;	/* Thread-global random seed */
__thread struct timespec msr_start, msr_end;
__thread double seconds_since_start;
__thread ::mica::util::ZipfGen *zg;	/* ZipfGen shared by all coroutines */

volatile int thread_barrier = 0;	/* Barrier after table population */

// Lockserver coroutine
void lockserver_func(coro_yield_t &yield)
{
	assert(rpc != NULL);

	int iters = 0;

	while(1) {
		iters++;
		next_coro = rpc->poll_comps();
		assert(next_coro[0] == RPC_MASTER_CORO_ID);

		if(iters == 2000000 && RPC_COLLECT_STATS == 1) {
			rpc->print_stats();
			iters = 0;
		}
	}
}

// Non-lockserver coroutines
void master_func(coro_yield_t &yield, int coro_id)
{
	assert(rpc != NULL);
	assert(coro_id == RPC_MASTER_CORO_ID);

	// Allow each slave to run once

	/* Use RPC's next_coro structure for initial coroutine spawning */
	next_coro = rpc->get_next_coro_arr();
	for(int coro_i = 0; coro_i < num_coro; coro_i++) {
		next_coro[coro_i] = (coro_i == num_coro - 1) ? 0 : coro_i + 1;
	}

	yield(coro_arr[1]);

	while(1) {
		next_coro = rpc->poll_comps();
		coro_id_t next_coro_id = next_coro[RPC_MASTER_CORO_ID];

		if(next_coro_id != RPC_MASTER_CORO_ID) {
			yield(coro_arr[next_coro_id]);
		}
	}
}

// Similar to slave_func(), but optimized for single-key RO txns
void slave_func_single_read(coro_yield_t &yield, int coro_id)
{
	assert(coro_id != RPC_MASTER_CORO_ID);
	assert(rpc != NULL);
	assert(read_set_size == 1 && write_percentage == 0);

	struct timespec tx_start_time, tx_end_time; /* Txn latency */
	_unused(tx_start_time); _unused(tx_end_time);	/* If MEASURE_LATENCY = 0 */

	/* DO NOT use rpc after this point. It belongs to tx/ now */
	Tx *tx = new Tx(coro_id, rpc, mappings, logger, coro_arr);

	hots_key_t key;	/* The single key */
	hots_obj_t obj;	/* The single object */

	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {
#if USE_ZIPF == 1
		key = zg->next();
#else
		key = hrd_fastrand(&tg_seed) & (num_keys_global - 1);
#endif

		stat_tx_tot++;

#if MEASURE_LATENCY == 1
		clock_gettime(CLOCK_REALTIME, &tx_start_time);
#endif

		/* Now we have the transaction's keys. Try it until we succeed. */
retry:
		tx->start();
		txtest_dprintf("Worker %d: Tx start\n", wrkr_gid);

		tx->add_to_read_set(RPC_MICA_REQ, key, &obj);

		tx_status_t ex_result = tx->do_read(yield);
		txtest_dprintf("Worker %d: Tx execute done\n", wrkr_gid);
		if(ex_result != tx_status_t::in_progress) {
			txtest_dprintf("Worker %d: Tx execute fail %lu\n",
				wrkr_gid, stat_ex_fail);
			stat_ex_fail++;
			tx->abort(yield);
			goto retry;
		} else {
			tx->commit_single_read();
			stat_commit_success++;
		}

		/* Transaction done. Measure latency. */
#if MEASURE_LATENCY == 1
		clock_gettime(CLOCK_REALTIME, &tx_end_time);
		double tx_usec = (tx_end_time.tv_sec - tx_start_time.tv_sec) * 1000000 +
			(double) (tx_end_time.tv_nsec - tx_start_time.tv_nsec) / 1000;
		stat_tx_tot_usec += tx_usec;
#endif
		
		/* Any coroutine can print the measurement results */
		if((stat_tx_tot & M_4_) == M_4_) {
			clock_gettime(CLOCK_REALTIME, &msr_end);

			/* Each of the two calls below zeroes out the corresponding stat */
			long long num_reqs = rpc->get_stat_num_reqs();
			long long num_creqs = rpc->get_stat_num_creqs();

			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;
			seconds_since_start += (msr_usec / 1000000);

			printf("Worker %d: Tx/s = %.5f M, "
				"reqs/s = {%.3f M, %.3f M coalesced}, "
				"avg latency = %.1f us, tx commit/s = %.5f M (fraction = %.2f), "
				"execution failed = %lu, commit failed = %lu, "
				"Tx stats = %s\n",
				wrkr_lid, stat_tx_tot / msr_usec,
				num_reqs / msr_usec, num_creqs / msr_usec,
				stat_tx_tot_usec / stat_tx_tot,
				stat_commit_success / msr_usec,
				(double) stat_commit_success / stat_tx_tot,
				stat_ex_fail, stat_commit_fail,
				tx->get_stats().c_str());
			fflush(stdout);

			/* Fill in this worker's global stats */
			auto &gs = global_stats[wrkr_lid];
			gs.tx_tput = (double) stat_commit_success / msr_usec;
			gs.req_rate = (double) num_reqs / msr_usec;
			gs.creq_rate = (double) num_creqs / msr_usec;
			if(seconds_since_start >= 30.0) {
				gs.max_lat_thread = rpc->get_max_batch_latency_us();
			} else {
				rpc->reset_max_batch_latency();
			}

			if(wrkr_lid == 0) {
				double tx_tput_tot = 0;
				double req_rate_tot = 0;
				double creq_rate_tot = 0;
				double max_lat_node = 0;	/* Max latency among all threads */

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					auto &gs_i = global_stats[wrkr_i];	/* Stats of worker i */
					tx_tput_tot += gs_i.tx_tput;
					req_rate_tot += gs_i.req_rate;
					creq_rate_tot += gs_i.creq_rate;

					if(gs_i.max_lat_thread > max_lat_node) {
						max_lat_node = gs_i.max_lat_thread;
					}
				}

				hrd_red_printf("Machine commit tput = %.2f M/s, "
					"req rate = {%.2f M/s, %.2f M/s coalesced}. "
					"Maximum batch latency = %.1f ms. Runtime = %.2f s\n",
					tx_tput_tot, req_rate_tot, creq_rate_tot,
					max_lat_node / 1000, seconds_since_start);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);

			rpc->print_stats();

			stat_tx_tot = 0;
			stat_tx_tot_usec = 0;
			stat_ex_fail = 0;
			stat_commit_fail = 0;
			stat_commit_success = 0;
		}
	}
}

// For general read-write transactions
void slave_func(coro_yield_t &yield, int coro_id)
{
	assert(coro_id != RPC_MASTER_CORO_ID);
	assert(rpc != NULL);

	struct timespec tx_start_time, tx_end_time; /* Txn latency */
	_unused(tx_start_time); _unused(tx_end_time);	/* If MEASURE_LATENCY = 0 */

	/* DO NOT use rpc after this point. It belongs to tx/ now */
	Tx *tx = new Tx(coro_id, rpc, mappings, logger, coro_arr);

	hots_key_t *key_arr = new hots_key_t[read_set_size];	/* Input to tx */
	hots_obj_t *obj_arr = new hots_obj_t[read_set_size];	/* Output from tx/ */
	bool *is_write = new bool[read_set_size];

	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {
retry:
		/* Now we have the transaction's keys. Try it until we succeed. */
		txtest_dprintf("Worker %d: Tx start\n", wrkr_gid);
		tx->start();

		uint64_t machine_mask = 0;
		int primary_mn;

		/* Generate keys with primaries at different machines */
		for(int i = 0; i < read_set_size; i++) {
#if USE_ZIPF == 1
			key_arr[i] = zg->next();
#else
			key_arr[i] = hrd_fastrand(&tg_seed) & (num_keys_global - 1);
#endif
			assert(key_arr[i] >= 0 && key_arr[i] < (uint64_t) num_keys_global);

			is_write[i] = false;
			if(hrd_fastrand(&tg_seed) % 100 < (unsigned) write_percentage) {
				is_write[i] = true;
			}

			if(is_write[i]) {
				primary_mn = tx->add_to_write_set(RPC_MICA_REQ,
					key_arr[i], &obj_arr[i], tx_write_mode_t::update);
			} else {
				primary_mn = tx->add_to_read_set(RPC_MICA_REQ,
					key_arr[i], &obj_arr[i]);
			}

			assert(primary_mn <= 63);
			if(is_set(machine_mask, primary_mn)) {
				/*
				 * We cannot try and generate a different key for key_arr[i]
				 * since we've already modified the transaction's read/write
				 * set. We must start over.
				 */
				goto retry;
			} else {
				bit_set(machine_mask, primary_mn);
			}
		}

		/* If we are here, we have keys with primaries at different machines */
		stat_tx_tot++;
#if MEASURE_LATENCY == 1
		clock_gettime(CLOCK_REALTIME, &tx_start_time);
#endif
		tx_status_t ex_result = tx->do_read(yield);
		txtest_dprintf("Worker %d: Tx execute done\n", wrkr_gid);
		if(ex_result != tx_status_t::in_progress) {
			txtest_dprintf("Worker %d: Tx execute fail %lu\n",
				wrkr_gid, stat_ex_fail);
			stat_ex_fail++;
			tx->abort(yield);
			goto retry;
		} else {
			if(read_set_size == 1 && write_percentage == 0) {
				tx->commit_single_read();
				stat_commit_success++;
			} else {
				tx_status_t commit_result = tx->commit(yield);
				if(commit_result != tx_status_t::committed) {
					txtest_dprintf("Worker %d: Tx commit fail %lu\n",
						wrkr_gid, stat_commit_fail);
					stat_commit_fail++;
					goto retry;
				} else {
					txtest_dprintf("Worker %d: Tx commit success %lu\n",
						wrkr_gid, stat_commit_success);
					stat_commit_success++;
				}
			}
		}

		/* Transaction done. Measure latency. */
#if MEASURE_LATENCY == 1
		clock_gettime(CLOCK_REALTIME, &tx_end_time);
		double tx_usec = (tx_end_time.tv_sec - tx_start_time.tv_sec) * 1000000 +
			(double) (tx_end_time.tv_nsec - tx_start_time.tv_nsec) / 1000;
		stat_tx_tot_usec += tx_usec;
#endif
		
		/* Any coroutine can print the measurement results */
		if((stat_tx_tot & K_128_) == K_128_) {
			clock_gettime(CLOCK_REALTIME, &msr_end);

			/* Each of the two calls below zeroes out the corresponding stat */
			long long num_reqs = rpc->get_stat_num_reqs();
			long long num_creqs = rpc->get_stat_num_creqs();

			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;
			seconds_since_start += (msr_usec / 1000000);

			printf("Worker %d: Tx/s = %.5f M, "
				"reqs/s = {%.3f M, %.3f M coalesced}, "
				"avg latency = %.1f us, tx commit/s = %.5f M (fraction = %.2f), "
				"execution failed = %lu, commit failed = %lu, "
				"Tx stats = %s\n",
				wrkr_lid, stat_tx_tot / msr_usec,
				num_reqs / msr_usec, num_creqs / msr_usec,
				stat_tx_tot_usec / stat_tx_tot,
				stat_commit_success / msr_usec,
				(double) stat_commit_success / stat_tx_tot,
				stat_ex_fail, stat_commit_fail,
				tx->get_stats().c_str());
			fflush(stdout);

			/* Fill in this worker's global stats */
			auto &gs = global_stats[wrkr_lid];
			gs.tx_tput = (double) stat_commit_success / msr_usec;
			gs.req_rate = (double) num_reqs / msr_usec;
			gs.creq_rate = (double) num_creqs / msr_usec;
			if(seconds_since_start >= 30.0) {
				gs.max_lat_thread = rpc->get_max_batch_latency_us();
			} else {
				rpc->reset_max_batch_latency();
			}

			if(wrkr_lid == 0) {
				double tx_tput_tot = 0;
				double req_rate_tot = 0;
				double creq_rate_tot = 0;
				double max_lat_node = 0;	/* Max latency among all threads */

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					auto &gs_i = global_stats[wrkr_i];	/* Stats of worker i */
					tx_tput_tot += gs_i.tx_tput;
					req_rate_tot += gs_i.req_rate;
					creq_rate_tot += gs_i.creq_rate;

					if(gs_i.max_lat_thread > max_lat_node) {
						max_lat_node = gs_i.max_lat_thread;
					}
				}

				hrd_red_printf("Machine commit tput = %.2f M/s, "
					"req rate = {%.2f M/s, %.2f M/s coalesced}. "
					"Maximum batch latency = %.1f ms. Runtime = %.2f s\n",
					tx_tput_tot, req_rate_tot, creq_rate_tot,
					max_lat_node / 1000, seconds_since_start);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);

			rpc->print_stats();

			stat_tx_tot = 0;
			stat_tx_tot_usec = 0;
			stat_ex_fail = 0;
			stat_commit_fail = 0;
			stat_commit_success = 0;
		}
	}
}

// Parse configuration. Requires wrkr_gid to be set.
void parse_config()
{
	auto test_config =
		::mica::util::Config::load_file("tx-test.json").get("tx_test");

	num_coro = (int) test_config.get("num_coro").get_int64();
	base_port_index = (int) test_config.get("base_port_index").get_int64();
	num_ports = (int) test_config.get("num_ports").get_int64();
	num_qps = (int) test_config.get("num_qps").get_int64();
	postlist = (int) test_config.get("postlist").get_int64();
	numa_node = (int) test_config.get("numa_node").get_int64();
	num_machines = test_config.get("num_machines").get_int64();
	num_backups = test_config.get("num_backups").get_int64();
	workers_per_machine = test_config.get("workers_per_machine").get_int64();
	use_lock_server = test_config.get("use_lock_server").get_bool();
	num_keys_kilo = (size_t) test_config.get("num_keys_kilo").get_int64();
	val_size = (int) test_config.get("val_size").get_int64();
	zipf_theta = test_config.get("zipf_theta").get_double();
	read_set_size = test_config.get("read_set_size").get_int64();
	write_percentage = test_config.get("write_percentage").get_int64();
	
	assert(num_coro >= 2 && num_coro <= RPC_MAX_CORO);
	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(num_ports >= 1 && num_ports <= 8);
	assert(num_qps >= 1 && num_qps <= 4);
	assert(postlist >= 1 && postlist <= 64);
	assert(numa_node >= 0 && numa_node <= 3);
	assert(num_machines >= 0 && num_machines <= 256);
	assert(num_backups >= 0 && num_backups <= HOTS_MAX_BACKUPS);
	assert(workers_per_machine >= 1 && workers_per_machine <= 56);
	assert(num_keys_kilo >= 1 && num_keys_kilo <= 8192);
	assert(val_size >= 1 && val_size <= HOTS_MAX_VALUE);
	assert(zipf_theta >= 0 && zipf_theta <= .99);
	assert(read_set_size >= 1 && read_set_size <= RPC_MAX_MSG_CORO);
	assert(write_percentage >= 0 && write_percentage <= 100);

	/* Ensure that we can avoid coalescing */
	if(read_set_size > num_machines) {
		fprintf(stderr, "Read set size too large to avoid coalescing\n");
		assert(false);
	}

	if(num_machines > 63) {
		fprintf(stderr, "Nbr of machines too large for 64-bit machine_mask\n");
		assert(false);
	}

	// Derived parameters
	num_workers = num_machines * workers_per_machine;

	num_keys_global = num_keys_kilo * 1024 * num_workers;
	assert(num_keys_global <= (2ull * 1024 * 1024 * 1024)); /* 32-bit fastrnd */
	num_keys_global = rte_align64pow2(num_keys_global);

	wrkr_lid = wrkr_gid % workers_per_machine;
}

void init_rpc()
{
	/* Largest packets are generated during logging. The commit record */
	int max_pkt_size = Tx::max_pkt_size(read_set_size,  val_size);

	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		num_workers, workers_per_machine, num_coro,
		base_port_index, num_ports, num_qps, numa_node, postlist, max_pkt_size);

	rpc = new Rpc(_rpc_args);
}

void run_thread(struct thread_params *params)
{
	wrkr_gid = params->wrkr_gid;
	global_stats = params->global_stats;

	parse_config();

	/* Use a different random number sequence for each thread */
	tg_seed = 0xdeadbeef + wrkr_gid;

	mappings = new Mappings(wrkr_gid,
		num_machines, workers_per_machine, num_backups, use_lock_server);
	logger = new Logger(wrkr_gid, wrkr_lid, num_machines, num_coro);

	printf("Worker %d: starting. Am I lock server = %d\n",
		wrkr_gid, mappings->am_i_lock_server);

	if(!use_lock_server || !mappings->am_i_lock_server) {
		// Initialization for non-lockserver workers
		assert(params->lockserver == NULL);

		/* Initialize Zipf generator */
		uint64_t zipf_seed = 2 * wrkr_gid * ::mica::util::rdtsc();
		uint64_t zipf_seed_mask = (uint64_t(1) << 48) - 1;
		zg = new ::mica::util::ZipfGen(num_keys_global,
			zipf_theta, zipf_seed & zipf_seed_mask);

		/*
		 * Populate this worker's pre-initialized FixedTable partition. The
		 * keys used for population don't depend on Zipf use.
		 */
		for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
			/* Populate with keys for which @wrkr_gid is replica  @repl_i.*/
			printf("Worker %d: Populating FixedTable. Total swarm keys = %lu\n",
				wrkr_gid, num_keys_global);
			ds_fixedtable_populate(params->fixedtable[repl_i],
				num_keys_global, val_size, mappings, repl_i,
				wrkr_gid,	/* = partition ID */
				true);	/* Only add keys that belong to this partition */
		}

		/*
		 * Expose this thread to RPCs only after this machine's partition is
		 * fully populated.
		 */
		__sync_fetch_and_add(&thread_barrier, 1);
		while(thread_barrier != workers_per_machine) {
			sleep(.1);
		}

		init_rpc();

		for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
			rpc->register_rpc_handler(RPC_MICA_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) params->fixedtable[repl_i]);
		}

		/* Register logger */
		rpc->register_rpc_handler(RPC_LOGGER_REQ,
			logger_rpc_handler, (void *) logger);

		/* Initialize coroutines */
		coro_arr = new coro_call_t[num_coro];
		for(int coro_i = 0; coro_i < num_coro; coro_i++) {
			if(coro_i == RPC_MASTER_CORO_ID) {
				coro_arr[coro_i] = coro_call_t(bind(master_func, _1, coro_i),
					attributes(fpu_not_preserved));
			} else {
				if(read_set_size == 1 && write_percentage == 0) {
					coro_arr[coro_i] = coro_call_t(
						bind(slave_func_single_read, _1, coro_i),
						attributes(fpu_not_preserved));
				} else {
					coro_arr[coro_i] = coro_call_t(bind(slave_func, _1, coro_i),
						attributes(fpu_not_preserved));
				}
			}
		}

		/* Launch the master coroutine */
		coro_arr[RPC_MASTER_CORO_ID]();
	} else {
		assert(use_lock_server && mappings->am_i_lock_server);
		assert(params->lockserver != NULL);

		printf("Worker %d: I am a lock server\n", wrkr_gid);
		init_rpc();

#if USE_RPC_LAYER_LOCKSERVER == 1
		rpc->locksrv_loop(params->lockserver);
#else
		lockserver = params->lockserver;
		rpc->register_rpc_handler(RPC_LOCKSERVER_REQ,
			lockserver_rpc_handler, (void *) lockserver);

		coro_call_t lockserver_coro = coro_call_t(lockserver_func,
			attributes(fpu_not_preserved));
		lockserver_coro();
#endif
	}

	delete rpc;
}
