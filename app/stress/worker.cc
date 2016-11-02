#include <boost/coroutine/all.hpp>
#include <boost/bind.hpp>
#include <papi.h>

#include "main.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "mappings/mappings.h"
#include "tx/tx.h"
#include "hots.h"
#include "util/rte_memcpy.h"
#include "datastore/fixedtable/ds_fixedtable.h"

#define N 2	/* Number of keys in each transaction */
static_assert(N <= RPC_MAX_MSG_CORO, "");

__thread int wrkr_gid;	/* Global ID of this worker */
__thread int wrkr_lid;	/* Local ID of this worker */
__thread int num_coro, base_port_index, num_ports, num_qps, postlist, numa_node;
__thread int num_machines, workers_per_machine, num_workers;
__thread int num_backups;
__thread bool use_lock_server;

/* Application parameters */
__thread global_stats_t *global_stats;

/* High-level HoTS structures */
__thread coro_call_t *coro_arr;
__thread coro_id_t *next_coro;
__thread Rpc *rpc;
__thread Logger *logger;
__thread Mappings *mappings;
__thread Stress *stress;
__thread stress_txn_type_t *workgen_arr;

/* Stats */
__thread long long stat_tx_attempted_tot = 0;	/* Collected in non-debug */
/* Transactions that contribute to the spec's mean qualified throughput */
__thread long long stat_tx_committed_tot = 0;	/* Collected in non-debug */
__thread long long stat_tx_attempted[STRESS_TXN_TYPES] = {0};
__thread long long stat_tx_commit_attempted[STRESS_TXN_TYPES] = {0};
__thread long long stat_tx_committed[STRESS_TXN_TYPES] = {0};

/* Check if a coroutine stops issuing transactions (due to any packet loss) */
__thread size_t stat_coro_attempted[RPC_MAX_CORO] = {0};
__thread double stat_tx_usec_tot = 0;

__thread uint64_t tg_seed = 0xdeadbeef;	/* Thread-global random seed */
__thread struct timespec msr_start, msr_end;

/*
 * Return the percentage of committed transactions wrt total transaction
 * attempts. Includes execute-time aborts due to missing read set keys, locked
 * write set keys, and inability to satisfy application predicates.
 */
double success_pct(stress_txn_type_t txn_type)
{
#if STRESS_COLLECT_STATS == 1
	int _txn_type = static_cast<int>(txn_type);

	return (double) 100 * stat_tx_committed[_txn_type] /
		stat_tx_attempted[_txn_type];
#else
	return -1.0;
#endif
}

/*
 * Return the percentage of committed transactions wrt total transactions that
 * reached the commit phase.
 */
double commit_pct(stress_txn_type_t txn_type)
{
#if STRESS_COLLECT_STATS == 1
	int _txn_type = static_cast<int>(txn_type);

	return (double) 100 * stat_tx_committed[_txn_type] /
		stat_tx_commit_attempted[_txn_type];
#else
	return -1.0;
#endif
}

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

/* Returns a key K such that K % @N == 0, and K + @N < @num_rows_total */
uint64_t get_base_key()
{
	uint64_t ret = hrd_fastrand(&tg_seed) % stress->num_rows_total;

	/* The largest key returned is < (@num_rows_total - @N) */
	while(ret % N != 0 || ret >= stress->num_rows_total - N) {
		ret = hrd_fastrand(&tg_seed) % stress->num_rows_total;
	}

	return ret;
}

void txn_get_N(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(stress_txn_type_t::get_N);
	tx->start();
	stress_stat_inc(stat_tx_attempted[txn_type], 1);
	stat_tx_attempted_tot++;

	stress_key_t stress_keys[N];
	hots_obj_t stress_objs[N];
	uint64_t base_key = get_base_key();
	for(size_t i = 0; i < N; i++) {
		/* Set val_size and timestamp */
		memset(&stress_objs[i], 0, 2 * sizeof(uint64_t));

		stress_keys[i].key = base_key + i;
		tx->add_to_read_set(RPC_STRESS_TABLE_REQ,
			stress_keys[i].hots_key, &stress_objs[i]);
	}

	tx_status_t ex_result = tx->do_read(yield);	_unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		/* Abort if we read a locked row */
		tx->abort_rdonly();
		return;
	}

	hots_obj_t execute_objs[N];
	size_t num_exists = 0;
	for(size_t i = 0; i < N; i++) {
		/* Save the execute-time object metadata to report in case of failure */
		execute_objs[i].ts = stress_objs[i].ts;
		execute_objs[i].val_size = stress_objs[i].val_size;

		if(stress_objs[i].val_size > 0) {
			num_exists++;
		}
	}
	
	stress_stat_inc(stat_tx_commit_attempted[txn_type], 1);
	tx_status_t commit_result = tx->commit(yield);

	if(commit_result == tx_status_t::committed) {
		stress_stat_inc(stat_tx_committed[txn_type], 1);

		/* The two correctness conditions; both can't be true at once */
		bool partial_objects = (num_exists != N && num_exists != 0);
		bool value_mismatch = false;

		if(num_exists == N) {
			/* If we found a key, all values should match */
			auto *stress_val_0 = (stress_val_t *) &stress_objs[0].val;
			for(size_t i = 1; i < N; i++) {
				auto *stress_val_i = (stress_val_t *) &stress_objs[i].val;
				if(stress_val_0->val != stress_val_i->val) {
					value_mismatch = true;
				}
			}
		}

		if(partial_objects || value_mismatch) {
			/* Print object metadata if there is an error */
			std::string execute_objs_string;
			std::string validate_objs_string;

			for(size_t i = 0; i < N; i++) {
				execute_objs_string +=
					execute_objs[i].to_string(stress_keys[i].key);
				execute_objs_string += ", ";
			}

			for(size_t i = 0; i < N; i++) {
				validate_objs_string +=
					stress_objs[i].to_string(stress_keys[i].key);
				validate_objs_string += ", ";
			}
	
			hrd_red_printf("Worker %d: Error: GET_%d observed error %s.\n"
				"Exec: %s\n"
				"Vald: %s\n", wrkr_gid, N,
				partial_objects ? "PARTIAL OBJECTS" : "VALUE MISMATCH",
				execute_objs_string.c_str(), validate_objs_string.c_str());

			if(value_mismatch) {
				/* Print the application-level values if values mismatch */
				std::string val_string;
				for(size_t i = 0; i < N; i++) {
					auto *_stress_val_i =
						(stress_val_t *) &stress_objs[i].val;
					val_string += std::to_string(_stress_val_i->val);
					val_string += ", ";
				}

				hrd_red_printf("Mismatched values: %s\n", val_string.c_str());
			}

			exit(-1);
		}

		stat_tx_committed_tot++;
	}
}

void txn_del_N(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(stress_txn_type_t::del_N);
	tx->start();
	stress_stat_inc(stat_tx_attempted[txn_type], 1);
	stat_tx_attempted_tot++;

	stress_key_t stress_keys[N];
	hots_obj_t stress_objs[N];
	uint64_t base_key = get_base_key();
	for(size_t i = 0; i < N; i++) {
		stress_keys[i].key = base_key + i;
		tx->add_to_write_set(RPC_STRESS_TABLE_REQ,
			stress_keys[i].hots_key, &stress_objs[i], tx_write_mode_t::del);
	}

	tx_status_t ex_result = tx->do_read(yield);	_unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		/* Abort if we read a locked or non-existent row */
		tx->abort(yield);
		return;
	}

	stress_stat_inc(stat_tx_commit_attempted[txn_type], 1);
	tx_status_t commit_result = tx->commit(yield);
	if(commit_result == tx_status_t::committed) {
		stress_stat_inc(stat_tx_committed[txn_type], 1);
		stat_tx_committed_tot++;
	}
}

void txn_ins_N(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(stress_txn_type_t::ins_N);
	tx->start();
	stress_stat_inc(stat_tx_attempted[txn_type], 1);
	stat_tx_attempted_tot++;

	stress_key_t stress_keys[N];
	hots_obj_t stress_objs[N];
	uint64_t base_key = get_base_key();
	for(size_t i = 0; i < N; i++) {
		stress_keys[i].key = base_key + i;
		tx->add_to_write_set(RPC_STRESS_TABLE_REQ,
			stress_keys[i].hots_key, &stress_objs[i], tx_write_mode_t::insert);
	}

	tx_status_t ex_result = tx->do_read(yield);	_unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		/* Abort if we read a locked or non-existent row */
		tx->abort(yield);
		return;
	}

	/* Construct the real object to insert here */
	uint64_t new_val = hrd_fastrand(&tg_seed);	/* All objs get same value */
	for(size_t i = 0; i < N; i++) {
		hots_format_real_obj(stress_objs[i], sizeof(stress_val_t));
		auto *stress_val = (stress_val_t *) &stress_objs[i].val;
		stress_val->val = new_val;
	}

	stress_stat_inc(stat_tx_commit_attempted[txn_type], 1);
	tx_status_t commit_result = tx->commit(yield);
	if(commit_result == tx_status_t::committed) {
		stress_stat_inc(stat_tx_committed[txn_type], 1);
		stat_tx_committed_tot++;
	}
}

void slave_func(coro_yield_t &yield, int coro_id)
{
	assert(coro_id != RPC_MASTER_CORO_ID);
	assert(rpc != NULL);

	struct timespec tx_start_time, tx_end_time; /* Txn latency */
	_unused(tx_start_time); _unused(tx_end_time);	/* Only if COLLECT_STATS */

	/* DO NOT use rpc after this point. It belongs to tx/ now */
	Tx *tx = new Tx(coro_id, rpc, mappings, logger, coro_arr);

	uint8_t magic __attribute__((unused)) = wrkr_gid + coro_id;

	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {
#if STRESS_COLLECT_STATS == 1
		clock_gettime(CLOCK_REALTIME, &tx_start_time);
#endif

		stat_coro_attempted[coro_id]++;
		
		stress_txn_type_t txn_type = workgen_arr[hrd_fastrand(&tg_seed) % 100];
		switch(txn_type) {
			case stress_txn_type_t::get_N:
				txn_get_N(yield, coro_id, tx);
				break;
			case stress_txn_type_t::del_N:
				txn_del_N(yield, coro_id, tx);
				break;
			case stress_txn_type_t::ins_N:
				txn_ins_N(yield, coro_id, tx);
				break;
			default:
				printf("Unexpected transaction type %d\n",
					static_cast<int>(txn_type));
				exit(-1);
		}
		
#if STRESS_COLLECT_STATS == 1
		/* Transaction done. Measure latency. */
		clock_gettime(CLOCK_REALTIME, &tx_end_time);
		double tx_usec = (tx_end_time.tv_sec - tx_start_time.tv_sec) * 1000000 +
			(double) (tx_end_time.tv_nsec - tx_start_time.tv_nsec) / 1000;
		stat_tx_usec_tot += tx_usec;
#endif
		
		/* Any coroutine can print the measurement results */
		if((stat_tx_attempted_tot & M_1_) == M_1_) {
			clock_gettime(CLOCK_REALTIME, &msr_end);

			/* Each of the two calls below zeroes out the corresponding stat */
			long long num_reqs = rpc->get_stat_num_reqs();
			long long num_creqs = rpc->get_stat_num_creqs();

			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;

			/* Fill in this worker's global stats */
			auto &gs = global_stats[wrkr_lid];
			gs.tx_tput = (double) stat_tx_committed_tot / msr_usec;
			gs.req_rate = (double) num_reqs / msr_usec;
			gs.creq_rate = (double) num_creqs / msr_usec;

			/* Compute min and max transactions attempted by coroutines */
			int min_id = 1, max_id = 1;
			_unused(min_id); _unused(max_id);	/* Debug mode only */
			size_t min_attempted = M_1024, max_attempted = 0;
			for(size_t i = 1; i < (unsigned) num_coro; i++) {
				if(stat_coro_attempted[i] < min_attempted) {
					min_attempted = stat_coro_attempted[i];
					min_id = i;
				}

				if(stat_coro_attempted[i] > max_attempted) {
					max_attempted = stat_coro_attempted[i];
					max_id = i;
				}
			}

#if STRESS_COLLECT_STATS == 1
			printf("Worker %d: attempted Tx/s = %.3f M, Commit tput = %.3f M, "
				"messages/s = {%.1f M, %.1f M coalesced}, avg latency = %.1f us, "
				"{Txn, success rate, commit rate}: {GET_N: %.3f, %.3f}, "
				"{DEL_N: %.3f, %.3f}, {INS_N: %.3f, %.3f}. "
				"Min attempted: {%d, %lu}, Max attempted: {%d, %lu}\n",
				wrkr_lid, stat_tx_attempted_tot / msr_usec,
				gs.tx_tput, gs.req_rate, gs.creq_rate,
				stat_tx_usec_tot / stat_tx_attempted_tot,
				success_pct(stress_txn_type_t::get_N),
				commit_pct(stress_txn_type_t::get_N),
				success_pct(stress_txn_type_t::del_N),
				commit_pct(stress_txn_type_t::del_N),
				success_pct(stress_txn_type_t::ins_N),
				commit_pct(stress_txn_type_t::ins_N),
				min_id, min_attempted, max_id, max_attempted);
#else
			printf("Worker %d: attempted Tx/s = %.3f M, Commit tput = %.3f M, "
				"messages/s = {%.1f M, %.1f M coalesced}\n",
				wrkr_gid, stat_tx_attempted_tot / msr_usec,
				gs.tx_tput, gs.req_rate, gs.creq_rate);
#endif

			if(wrkr_lid == 0) {
				double tx_tput_tot = 0;
				double req_rate_tot = 0;
				double creq_rate_tot = 0;

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					tx_tput_tot += global_stats[wrkr_i].tx_tput;
					req_rate_tot += global_stats[wrkr_i].req_rate;
					creq_rate_tot += global_stats[wrkr_i].creq_rate;
				}

				hrd_red_printf("Commit tput = %.2f M/s, "
					"req rate = {%.2f M/s, %.2f M/s coalesced}.\n",
					tx_tput_tot, req_rate_tot, creq_rate_tot);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);

			stat_tx_attempted_tot = 0;
			stat_tx_committed_tot = 0;
			memset((void *) stat_tx_attempted,
				0, STRESS_TXN_TYPES * sizeof(long long));
			memset((void *) stat_tx_committed,
				0, STRESS_TXN_TYPES * sizeof(long long));
			memset((void *) stat_tx_commit_attempted,
				0, STRESS_TXN_TYPES * sizeof(long long));

			memset((void *) stat_coro_attempted,
				0, RPC_MAX_CORO * sizeof(size_t));
			stat_tx_usec_tot = 0;
		}
	}
}

void parse_config()
{
	// Parse configuration
	auto test_config = ::mica::util::Config::load_file(
		"stress.json").get("hots");

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
	
	assert(num_coro >= 2 && num_coro <= RPC_MAX_CORO);
	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(num_ports >= 1 && num_ports <= 8);
	assert(num_qps >= 1 && num_qps <= 4);
	assert(postlist >= 1 && postlist <= 64);
	assert(numa_node >= 0 && numa_node <= 3);
	assert(num_machines >= 0 && num_machines <= 256);
	assert(num_backups >= 0 && num_backups <= HOTS_MAX_BACKUPS);
	assert(workers_per_machine >= 1 && workers_per_machine <= 56);
	assert(use_lock_server == false);	/* For now */

	// Derived parameters
	num_workers = num_machines * workers_per_machine;

	wrkr_lid = wrkr_gid % workers_per_machine;
}

void run_thread(struct thread_params *params)
{
	wrkr_gid = params->wrkr_gid;
	stress = params->stress;
	global_stats = params->global_stats;

	parse_config();

	/* Use a different random number sequence for each thread */
	tg_seed = 0xdeadbeef + wrkr_gid;

	mappings = new Mappings(wrkr_gid,
		num_machines, workers_per_machine, num_backups, use_lock_server);
	logger = new Logger(wrkr_gid, wrkr_lid, num_machines, num_coro);

	/*
	 * Populate tables before creating RPC endpoints. This is required because
	 * the thread that populates a key may be different from the thread that
	 * is responsible for serving the key at run time.
	 */
	printf("Worker %d: populating table.\n", wrkr_gid);
	stress->populate_all_tables_barrier(mappings);
	workgen_arr = stress->create_workgen_array();
	hrd_red_printf("Worker %d: populated all tables\n", wrkr_gid);

	// Initialize Rpc

	int max_pkt_size = Tx::max_pkt_size(N, sizeof(stress_val_t));

	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		num_workers, workers_per_machine, num_coro,
		base_port_index, num_ports, num_qps, numa_node, postlist, max_pkt_size);
	rpc = new Rpc(_rpc_args);

	stress->register_rpc_handlers(rpc);

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
			coro_arr[coro_i] = coro_call_t(bind(slave_func, _1, coro_i),
				attributes(fpu_not_preserved));
		}
	}

	printf("Worker %d: starting transactions!\n", wrkr_gid);

	/* Launch the master coroutine */
	coro_arr[RPC_MASTER_CORO_ID]();

	delete rpc;
}
