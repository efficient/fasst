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
#include "mica/util/latency.h"
#include "datastore/fixedtable/ds_fixedtable.h"

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
__thread SB *sb;
__thread sb_txn_type_t *workgen_arr;

/* Stats */
__thread long long stat_tx_attempted_tot = 0;	/* Collected in non-debug */
/* Transactions that contribute to the spec's mean qualified throughput */
__thread long long stat_tx_committed_tot = 0;	/* Collected in non-debug */
__thread long long stat_tx_attempted[SB_TXN_TYPES];
__thread long long stat_tx_committed[SB_TXN_TYPES];

__thread uint64_t tg_seed = 0xdeadbeef;	/* Thread-global random seed */
__thread struct timespec msr_start, msr_end;
__thread ::mica::util::Latency *latency; /* hl's amazing latency measurement */

/*
 * Return the percentage of committed transactions wrt total transaction
 * attempts. Includes execute-time aborts due to missing read set keys, locked
 * write set keys, and inability to satisfy application predicates.
 */
double get_txn_success_percent(sb_txn_type_t txn_type)
{
#if SB_COLLECT_STATS == 1
	int _txn_type = static_cast<int>(txn_type);

	return (double) 100 * stat_tx_committed[_txn_type] /
		stat_tx_attempted[_txn_type];
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

void txn_amalgamate(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::amalgamate;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters */
	uint64_t acct_id_0, acct_id_1;
	sb->get_two_accounts(&tg_seed, &acct_id_0, &acct_id_1);

	/* Read from savings and checking tables for acct_id_0 */
	hots_obj_t sav_obj_0;
	sb_sav_key_t sav_key_0;
	sav_key_0.acct_id = acct_id_0;
	tx->add_to_write_set(RPC_SAVING_REQ,
		sav_key_0.hots_key, &sav_obj_0, tx_write_mode_t::update);

	hots_obj_t chk_obj_0;
	sb_chk_key_t chk_key_0;
	chk_key_0.acct_id = acct_id_0;
	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key_0.hots_key, &chk_obj_0, tx_write_mode_t::update);

	/* Read from checking account for acct_id_1 */
	hots_obj_t chk_obj_1;
	sb_chk_key_t chk_key_1;
	chk_key_1.acct_id = acct_id_1;
	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key_1.hots_key, &chk_obj_1, tx_write_mode_t::update);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);

	sb_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Found or locked */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return;
	}

	/* If we are here, execution succeeded and we have locks */
	sb_sav_val_t *sav_val_0 = (sb_sav_val_t *) sav_obj_0.val;
	sb_chk_val_t *chk_val_0 = (sb_chk_val_t *) chk_obj_0.val;
	sb_chk_val_t *chk_val_1 = (sb_chk_val_t *) chk_obj_1.val;
	sb_dassert(sav_val_0->magic == sb_sav_magic);
	sb_dassert(chk_val_0->magic == sb_chk_magic);
	sb_dassert(chk_val_1->magic == sb_chk_magic);

	/* Increase acct_id_1's balance and set acct_id_0's balances to 0 */
	chk_val_1->bal += (sav_val_0->bal + chk_val_0->bal);
	
	sav_val_0->bal = 0;
	chk_val_0->bal = 0;

	tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
	/* if we managed to lock the records, the txn must commit */
	sb_dassert(commit_status == tx_status_t::committed);

	stat_tx_committed_tot++;
	sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
}

/* Calculate the sum of saving and checking balance */
void txn_balance(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::balance;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters */
	uint64_t acct_id;
	sb->get_account(&tg_seed, &acct_id);

	/* Read from savings and checking tables */
	hots_obj_t sav_obj;
	sb_sav_key_t sav_key;
	sav_key.acct_id = acct_id;
	tx->add_to_read_set(RPC_SAVING_REQ, sav_key.hots_key, &sav_obj);

	hots_obj_t chk_obj;
	sb_chk_key_t chk_key;
	chk_key.acct_id = acct_id;
	tx->add_to_read_set(RPC_CHECKING_REQ, chk_key.hots_key, &chk_obj);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		/* Abort if we read a locked  record */
		tx->abort_rdonly();
		return;
	}

	sb_sav_val_t *sav_val = (sb_sav_val_t *) sav_obj.val; _unused(sav_val);
	sb_chk_val_t *chk_val = (sb_chk_val_t *) chk_obj.val; _unused(chk_val);
	sb_dassert(sav_val->magic == sb_sav_magic);
	sb_dassert(chk_val->magic == sb_chk_magic);

	tx_status_t commit_result = tx->commit(yield);
	if(commit_result == tx_status_t::committed) {
		stat_tx_committed_tot++;
		sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
	} else {
		tx->abort_rdonly();
	}
}

/* Add $1.3 to acct_id's checking account */
void txn_deposit_checking(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::deposit_checking;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters */
	uint64_t acct_id;
	sb->get_account(&tg_seed, &acct_id);
	float amount = 1.3;

	/* Read from checking table */
	hots_obj_t chk_obj;
	sb_chk_key_t chk_key;
	chk_key.acct_id = acct_id;
	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key.hots_key, &chk_obj, tx_write_mode_t::update);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	sb_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Found or locked */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return;
	}

	/* If we are here, execution succeeded and we have a lock*/
	sb_chk_val_t *chk_val = (sb_chk_val_t *) chk_obj.val;
	sb_dassert(chk_val->magic == sb_chk_magic);

	chk_val->bal += amount;	/* Update checking balance */

	tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
	/* If we managed to lock the record, the txn must commit */
	sb_dassert(commit_status == tx_status_t::committed);

	stat_tx_committed_tot++;
	sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
}

/* Send $5 from acct_id_0's checking account to acct_id_1's checking account */
void txn_send_payment(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::send_payment;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters: send money from acct_id_0 to acct_id_1 */
	uint64_t acct_id_0, acct_id_1;
	sb->get_two_accounts(&tg_seed, &acct_id_0, &acct_id_1);
	float amount = 5.0;

	/* Read from checking table */
	hots_obj_t chk_obj_0, chk_obj_1;

	sb_chk_key_t chk_key_0, chk_key_1;
	chk_key_0.acct_id = acct_id_0;
	chk_key_1.acct_id = acct_id_1;

	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key_0.hots_key, &chk_obj_0, tx_write_mode_t::update);
	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key_1.hots_key, &chk_obj_1, tx_write_mode_t::update);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	sb_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Found or locked */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return;
	}

	/* if we are here, execution succeeded and we have locks */
	sb_chk_val_t *chk_val_0 = (sb_chk_val_t *) chk_obj_0.val;
	sb_chk_val_t *chk_val_1 = (sb_chk_val_t *) chk_obj_1.val;
	sb_dassert(chk_val_0->magic == sb_chk_magic);
	sb_dassert(chk_val_1->magic == sb_chk_magic);

	if(chk_val_0->bal < amount) {
		tx->abort(yield);
		return;
	}

	chk_val_0->bal -= amount;	/* Debit */
	chk_val_1->bal += amount;	/* Credit */

	tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
	/* if we managed to lock the records, the txn must commit */
	sb_dassert(commit_status == tx_status_t::committed);

	stat_tx_committed_tot++;
	sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
}

/* Add $20 to acct_id's saving's account */
void txn_transact_saving(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::transact_saving;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters */
	uint64_t acct_id;
	sb->get_account(&tg_seed, &acct_id);
	float amount = 20.20;

	/* Read from saving table */
	hots_obj_t sav_obj;
	sb_sav_key_t sav_key;
	sav_key.acct_id = acct_id;
	tx->add_to_write_set(RPC_SAVING_REQ,
		sav_key.hots_key, &sav_obj, tx_write_mode_t::update);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	sb_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Found or locked */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return;
	}

	/* If we are here, execution succeeded and we have a lock */
	sb_sav_val_t *sav_val = (sb_sav_val_t *) sav_obj.val;
	sb_dassert(sav_val->magic == sb_sav_magic);

	sav_val->bal += amount;	/* Update saving balance */

	tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
	/* If we managed to lock the record, the txn must commit */
	sb_dassert(commit_status == tx_status_t::committed);

	stat_tx_committed_tot++;
	sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
}

/* Read saving and checking balance + update checking balance unconditionally */
void txn_write_check(coro_yield_t &yield, int coro_id, Tx *tx)
{
	sb_txn_type_t txn_type = sb_txn_type_t::write_check;
	tx->start();
	sb_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);
	stat_tx_attempted_tot++;

	/* Transaction parameters */
	uint64_t acct_id;
	sb->get_account(&tg_seed, &acct_id);
	float amount = 5.0;

	/* Read from savings. Read checking record for update. */
	hots_obj_t sav_obj;
	sb_sav_key_t sav_key;
	sav_key.acct_id = acct_id;
	tx->add_to_read_set(RPC_SAVING_REQ, sav_key.hots_key, &sav_obj);

	hots_obj_t chk_obj;
	sb_chk_key_t chk_key;
	chk_key.acct_id = acct_id;
	tx->add_to_write_set(RPC_CHECKING_REQ,
		chk_key.hots_key, &chk_obj, tx_write_mode_t::update);

	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	sb_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Found or locked */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return;
	}

	sb_sav_val_t *sav_val = (sb_sav_val_t *) sav_obj.val;
	sb_chk_val_t *chk_val = (sb_chk_val_t *) chk_obj.val;
	sb_dassert(sav_val->magic == sb_sav_magic);
	sb_dassert(chk_val->magic == sb_chk_magic);

	if(sav_val->bal + chk_val->bal < amount) {
		chk_val->bal -= (amount + 1);
	} else {
		chk_val->bal -= amount;
	}

	tx_status_t commit_result = tx->commit(yield);

	/* This transaction can fail during validation */
	if(commit_result == tx_status_t::committed) {
		stat_tx_committed_tot++;
		sb_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);
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
#if SB_COLLECT_STATS == 1
		clock_gettime(CLOCK_REALTIME, &tx_start_time);
#endif
		
		sb_txn_type_t txn_type = workgen_arr[hrd_fastrand(&tg_seed) % 100];
		switch(txn_type) {
			case sb_txn_type_t::amalgamate:
				txn_amalgamate(yield, coro_id, tx);
				break;
			case sb_txn_type_t::balance:
				txn_balance(yield, coro_id, tx);
				break;
			case sb_txn_type_t::deposit_checking:
				txn_deposit_checking(yield, coro_id, tx);
				break;
			case sb_txn_type_t::send_payment:
				txn_send_payment(yield, coro_id, tx);
				break;
			case sb_txn_type_t::transact_saving:
				txn_transact_saving(yield, coro_id, tx);
				break;
			case sb_txn_type_t::write_check:
				txn_write_check(yield, coro_id, tx);
				break;
			default:
				printf("Invalid txn type\n");
				assert(false);
				exit(-1);
		}
		
#if SB_COLLECT_STATS == 1
		/* Transaction done. Measure latency. */
		clock_gettime(CLOCK_REALTIME, &tx_end_time);
		double tx_usec = (tx_end_time.tv_sec - tx_start_time.tv_sec) * 1000000 +
			(double) (tx_end_time.tv_nsec - tx_start_time.tv_nsec) / 1000;
		latency->update(tx_usec);
#endif
		
		/* Any coroutine can print the measurement results */
		if((stat_tx_attempted_tot & K_128_) == K_128_) {
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

#if SB_COLLECT_STATS == 1
			printf("Worker %d: attempted Tx/s = %.3f M. Commit tput = %.3f M. "
				"Messages/s = {%.1f M, %.1f M coalesced}. "
				"Latency = {%u us median, %u us 99 percentile. "
				"Success rate: {AMG: %.1f, BAL: %.1f, DC: %.1f, SP: %.1f, "
				"TS: %.1f, WC: %.1f}\n",
				wrkr_gid, stat_tx_attempted_tot / msr_usec,
				gs.tx_tput, gs.req_rate, gs.creq_rate,
				(unsigned) latency->perc(.5), (unsigned) latency->perc(.99),
				get_txn_success_percent(sb_txn_type_t::amalgamate),
				get_txn_success_percent(sb_txn_type_t::balance),
				get_txn_success_percent(sb_txn_type_t::deposit_checking),
				get_txn_success_percent(sb_txn_type_t::send_payment),
				get_txn_success_percent(sb_txn_type_t::transact_saving),
				get_txn_success_percent(sb_txn_type_t::write_check));
#else
			printf("Worker %d: attempted Tx/s = %.3f M, Commit tput = %.3f M, "
				"messages/s = {%.1f M, %.1f M coalesced}\n",
				wrkr_gid, stat_tx_attempted_tot / msr_usec,
				gs.tx_tput, gs.req_rate, gs.creq_rate);
#endif
			fflush(stdout);

			if(wrkr_lid == 0) {
				double tx_tput_tot = 0;
				double req_rate_tot = 0;
				double creq_rate_tot = 0;

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					tx_tput_tot += global_stats[wrkr_i].tx_tput;
					req_rate_tot += global_stats[wrkr_i].req_rate;
					creq_rate_tot += global_stats[wrkr_i].creq_rate;
				}

				hrd_red_printf("Machine commit tput = %.2f M/s, "
					"req rate = {%.2f M/s, %.2f M/s coalesced}.\n",
					tx_tput_tot, req_rate_tot, creq_rate_tot);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);

			stat_tx_attempted_tot = 0;
			stat_tx_committed_tot = 0;
			memset((void *) stat_tx_attempted,
				0, SB_TXN_TYPES * sizeof(long long));
			memset((void *) stat_tx_committed,
				0, SB_TXN_TYPES * sizeof(long long));
			latency->reset();
		}
	}
}

void parse_config()
{
	// Parse configuration
	auto test_config = ::mica::util::Config::load_file("sb.json").get("hots");

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
	sb = params->sb;
	global_stats = params->global_stats;

	parse_config();

	/* Use a different random number sequence for each thread */
	tg_seed = 0xdeadbeef + wrkr_gid;

	/* Initialize latency measurement */
	latency = new ::mica::util::Latency();

	mappings = new Mappings(wrkr_gid,
		num_machines, workers_per_machine, num_backups, use_lock_server);
	logger = new Logger(wrkr_gid, wrkr_lid, num_machines, num_coro);

	/*
	 * Populate tables before creating RPC endpoints. This is required because
	 * the thread that populates a key may be different from the thread that
	 * is responsible for serving the key at run time.
	 */
	printf("Worker %d: populating SmallBank tables.\n", wrkr_gid);
	sb->populate_all_tables_barrier(mappings);
	workgen_arr = sb->create_workgen_array();
	hrd_red_printf("Worker %d: populated all tables\n", wrkr_gid);

	// Initialize Rpc
	int max_pkt_size = 400; /* XXX: What should this be? */

	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		num_workers, workers_per_machine, num_coro,
		base_port_index, num_ports, num_qps, numa_node, postlist, max_pkt_size);
	rpc = new Rpc(_rpc_args);

	sb->register_rpc_handlers(rpc);

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
