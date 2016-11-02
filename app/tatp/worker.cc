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
__thread TATP *tatp;
__thread tatp_txn_type_t *workgen_arr;

/* Stats */
__thread long long stat_tx_attempted_tot = 0;	/* Collected in non-debug */
/* Transactions that contribute to the spec's mean qualified throughput */
__thread long long stat_tx_committed_tot = 0;	/* Collected in non-debug */
__thread long long stat_tx_attempted[TATP_TXN_TYPES] = {0};
__thread long long stat_tx_commit_attempted[TATP_TXN_TYPES] = {0};
__thread long long stat_tx_committed[TATP_TXN_TYPES] = {0};

__thread uint64_t tg_seed = 0xdeadbeef;	/* Thread-global random seed */
__thread struct timespec msr_start, msr_end;
__thread ::mica::util::Latency *latency; /* hl's amazing latency measurement */
constexpr int lat_multiplier = 10;	/* For sub-microsecond latency measuremnt */

/*
 * Return the percentage of committed transactions wrt total transaction
 * attempts. Includes execute-time aborts due to missing read set keys, locked
 * write set keys, and inability to satisfy application predicates.
 */
double success_pct(tatp_txn_type_t txn_type)
{
#if TATP_COLLECT_STATS == 1
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
double commit_pct(tatp_txn_type_t txn_type)
{
#if TATP_COLLECT_STATS == 1
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

// Profile: Read 1 SUBSCRIBER row
// Maximum messages in a batch = 1
bool txn_get_subscriber_data(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::get_subsciber_data);
	tx->start();

	tatp_sub_key_t key;
	key.s_id = tatp->get_nurand_subscriber(&tg_seed);

	hots_obj_t obj;

	tx->add_to_read_set(RPC_SUBSCRIBER_REQ, key.hots_key, &obj);
	tx_status_t ex_result = tx->do_read(yield);	_unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		/* Abort if we read a locked Subscriber record */
		tx->abort_rdonly();
		return false;
	}

	tatp_sub_val_t *val = (tatp_sub_val_t *) &obj.val;
	_unused(val);
	tatp_dassert(obj.val_size > 0);
	tatp_dassert(val->msc_location == tatp_sub_msc_location_magic);

	tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
	tx->commit_single_read();

	return true;
}

// Profile:
// 1. Read 1 SPECIAL_FACILITY row
// 2. Read up to 3 CALL_FORWARDING rows
// 3. Validate up to 4 rows
// Maximum messages in a batch = 4
bool txn_get_new_destination(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::get_new_destination);
	tx->start();

	/* Transaction parameters */
	uint32_t s_id = tatp->get_nurand_subscriber(&tg_seed);
	uint8_t sf_type = (hrd_fastrand(&tg_seed) % 4) + 1;
	uint8_t start_time = (hrd_fastrand(&tg_seed) % 3) * 8;
	uint8_t end_time = (hrd_fastrand(&tg_seed) % 24) * 1;

	unsigned cf_to_fetch = (start_time / 8) + 1;
	tatp_dassert(cf_to_fetch >= 1 && cf_to_fetch <= 3);

	/* Fetch a single special facility record */
	hots_obj_t specfac_obj;
	tatp_specfac_key_t specfac_key;

	specfac_key.s_id = s_id;
	specfac_key.sf_type = sf_type;
	tx->add_to_read_set(RPC_SPECIAL_FACILITY_REQ,
		specfac_key.hots_key, &specfac_obj);

	/*
	 * The Special Facility record exists only 62.5% of the time, and is_active
	 * 85% of the time. So avoid issuing the Call Forwarding fetches if we
	 * have a non-existent or inactive Special Facility record.
	 */
	tx_status_t ex_result = tx->do_read(yield); _unused(ex_result);
	if(ex_result != tx_status_t::in_progress || specfac_obj.val_size == 0) {
		tx->abort_rdonly();
		return false;
	}

	/* If we are here, the Special Facility record exists. */
	tatp_specfac_val_t *specfac_val = (tatp_specfac_val_t *)
		&specfac_obj.val;
	tatp_dassert(specfac_val->data_b[0] == tatp_specfac_data_b0_magic);
	if(specfac_val->is_active == 0) {
		tx->abort_rdonly();
		return false;
	}

	/* Fetch possibly multiple call forwarding records. */
	hots_obj_t callfwd_obj[3];
	tatp_callfwd_key_t callfwd_key[3];

	for(unsigned i = 0; i < cf_to_fetch; i++) {
		callfwd_key[i].s_id = s_id;
		callfwd_key[i].sf_type = sf_type;
		callfwd_key[i].start_time = (i * 8);

		tx->add_to_read_set(RPC_CALL_FORWARDING_REQ,
			callfwd_key[i].hots_key, &callfwd_obj[i]);
	}

	ex_result = tx->do_read(yield);	_unused(ex_result);
	if(ex_result != tx_status_t::in_progress) {
		tx->abort_rdonly();
		return false;
	}

	bool callfwd_success = false;
	for(unsigned i = 0; i < cf_to_fetch; i++) {
		if(callfwd_obj[i].val_size == 0) {
			continue;
		}

		tatp_callfwd_val_t *callfwd_val = (tatp_callfwd_val_t *)
			&callfwd_obj[i].val;
		tatp_dassert(callfwd_val->numberx[0] == tatp_callfwd_numberx0_magic);

		if(callfwd_key[i].start_time <= start_time &&
			end_time < callfwd_val->end_time) {
			/* All conditions satisfied */
			callfwd_success = true;
		}
	}

	if(callfwd_success) {
		/* Try to commit, which may fail during validation */
		tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
		tx_status_t commit_result = tx->commit(yield);
		return (commit_result == tx_status_t::committed);
	} else {
		tx->abort_rdonly();
		return false;
	}
}

// Profile: Read 1 ACCESS_INFO row
// Maximum messages in a batch = 1
bool txn_get_access_data(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::get_access_data);
	tx->start();

	hots_obj_t obj;
	tatp_accinf_key_t key;
	key.s_id = tatp->get_nurand_subscriber(&tg_seed);
	key.ai_type = (hrd_fastrand(&tg_seed) & 3) + 1;

	tx->add_to_read_set(RPC_ACCESS_INFO_REQ, key.hots_key, &obj);
	tx_status_t ex_result = tx->do_read(yield);	_unused(ex_result);

	/*
	 * No transaction locks Access Info records. Txn status remains @in_progress
	 * regardless of whether <s_id, ai_type> is found.
	 */
	tatp_dassert(ex_result == tx_status_t::in_progress);

	if(obj.val_size > 0) {
		/* The key was found */
		tatp_accinf_val_t *val = (tatp_accinf_val_t *) &obj.val;
		_unused(val);
		tatp_dassert(val->data1 == tatp_accinf_data1_magic);

		tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
		tx->commit_single_read();
		return true;
	} else {
		/* Key not found */
		tx->abort_rdonly();
		return false;
	}
}

// Profile: Update 1 SUBSCRIBER row and 1 SPECIAL_FACILTY row
// Maximum messages in a batch = 4 (during commit-backup)
bool txn_update_subscriber_data(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::update_subscriber_data);
	tx->start();

	/* Transaction parameters */
	uint32_t s_id = tatp->get_nurand_subscriber(&tg_seed);
	uint8_t sf_type = (hrd_fastrand(&tg_seed) % 4) + 1;

	/* Read + lock the subscriber record */
	hots_obj_t sub_obj;
	tatp_sub_key_t sub_key;
	sub_key.s_id = s_id;

	tx->add_to_write_set(RPC_SUBSCRIBER_REQ,
		sub_key.hots_key, &sub_obj, tx_write_mode_t::update);

	/* Read + lock the special facilty record */
	hots_obj_t specfac_obj;
	tatp_specfac_key_t specfac_key;
	specfac_key.s_id = s_id;
	specfac_key.sf_type = sf_type;

	tx->add_to_write_set(RPC_SPECIAL_FACILITY_REQ,
		specfac_key.hots_key, &specfac_obj, tx_write_mode_t::update);
	
	tx_status_t ex_result = tx->do_read(yield);
	tatp_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* Locked or not found */

	if(ex_result == tx_status_t::must_abort) {
		tx->abort(yield);
		return false;
	}

	/* If we are here, execution succeeded and we have locks */
	tatp_sub_val_t *sub_val = (tatp_sub_val_t *) sub_obj.val;
	tatp_dassert(sub_val->msc_location == tatp_sub_msc_location_magic);
	sub_val->bits = hrd_fastrand(&tg_seed);	/* Update */

	tatp_specfac_val_t *specfac_val = (tatp_specfac_val_t *) specfac_obj.val;
	tatp_dassert(specfac_val->data_b[0] == tatp_specfac_data_b0_magic);
	specfac_val->data_a = hrd_fastrand(&tg_seed);	/* Update */

	tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
	tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
	/*
	 * If we managed to lock the SUBSCRIBER and SPECIAL_FACILTY record, the
	 * txn must commit.
	 */
	tatp_dassert(commit_status == tx_status_t::committed);

	return true;
}

// Profile:
// 1. Read a SECONDARY_SUBSCRIBER row
// 2. Update a SUBSCRIBER row
// Maximum messages in a batch = 2 (during execute, logging, commit-backup)
bool txn_update_location(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::update_location);
	tx->start();

	/* Transaction parameters */
	uint32_t s_id = tatp->get_nurand_subscriber(&tg_seed);
	uint32_t vlr_location = hrd_fastrand(&tg_seed);

	/* Read the secondary subscriber record */
	hots_obj_t sec_sub_obj;
	tatp_sec_sub_key_t sec_sub_key;
	sec_sub_key.sub_nbr = tatp->tatp_sub_nbr_from_sid_fast(s_id);

	tx->add_to_read_set(RPC_SEC_SUBSCRIBER_REQ,
		sec_sub_key.hots_key, &sec_sub_obj);
	tx_status_t ex_result = tx->do_read(yield);

	tatp_dassert(ex_result == tx_status_t::in_progress);	/* Never locked */
	tatp_dassert(sec_sub_obj.val_size > 0);	/* Must exist */

	tatp_sec_sub_val_t *sec_sub_val = (tatp_sec_sub_val_t *) sec_sub_obj.val;
	tatp_dassert(sec_sub_val->magic == tatp_sec_sub_magic);
	tatp_dassert(sec_sub_val->s_id == s_id);

	/* Read + lock the subscriber record */
	hots_obj_t sub_obj;
	tatp_sub_key_t sub_key;
	sub_key.s_id = sec_sub_val->s_id;

	tx->add_to_write_set(RPC_SUBSCRIBER_REQ,
		sub_key.hots_key, &sub_obj, tx_write_mode_t::update);
	ex_result = tx->do_read(yield);

	tatp_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* If locked */

	if(ex_result == tx_status_t::in_progress) {
		tatp_sub_val_t *sub_val = (tatp_sub_val_t *) sub_obj.val;
		tatp_dassert(sub_val->msc_location == tatp_sub_msc_location_magic);
		sub_val->vlr_location = vlr_location;	/* Update */
			
		tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
		tx_status_t commit_status = tx->commit(yield); _unused(commit_status);
	
		/*
		 * If we managed to lock the SUBSCRIBER record, the txn must commit.
		 * (Validation must succeed because the secondary table is read-only.)
		 */
		tatp_dassert(commit_status == tx_status_t::committed);
		return true;
	} else {
		tx->abort(yield);
		return false;
	}
}

// Profile:
// 1. Read a SECONDARY_SUBSCRIBER row
// 2. Read a SPECIAL_FACILTY row
// 3. Insert a CALL_FORWARDING row
// Maximum messages in a batch = 2 (during validate and commit-backup)
bool txn_insert_call_forwarding(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::insert_call_forwarding);
	tx->start();

	/* Transaction parameters */
	uint32_t s_id = tatp->get_nurand_subscriber(&tg_seed);
	uint8_t sf_type = (hrd_fastrand(&tg_seed) % 4) + 1;
	uint8_t start_time = (hrd_fastrand(&tg_seed) % 3) * 8;
	uint8_t end_time = (hrd_fastrand(&tg_seed) % 24) * 1;

	// Read the secondary subscriber record
	hots_obj_t sec_sub_obj;
	tatp_sec_sub_key_t sec_sub_key;
	sec_sub_key.sub_nbr = tatp->tatp_sub_nbr_from_sid_fast(s_id);

	tx->add_to_read_set(RPC_SEC_SUBSCRIBER_REQ,
		sec_sub_key.hots_key, &sec_sub_obj);
	tx_status_t ex_result = tx->do_read(yield);
	tatp_dassert(ex_result == tx_status_t::in_progress);	/* Never locked */

	tatp_dassert(sec_sub_obj.val_size > 0);	/* Must exist */

	auto *sec_sub_val = (tatp_sec_sub_val_t *) sec_sub_obj.val;
	_unused(sec_sub_val);
	tatp_dassert(sec_sub_val->magic == tatp_sec_sub_magic);
	tatp_dassert(sec_sub_val->s_id == s_id);

	// Read the Special Facility record
	hots_obj_t specfac_obj;
	tatp_specfac_key_t specfac_key;
	specfac_key.s_id = s_id;
	specfac_key.sf_type = sf_type;
	tx->add_to_read_set(RPC_SPECIAL_FACILITY_REQ,
		specfac_key.hots_key, &specfac_obj);

	/* The Special Facility record exists only 62.5% of the time */
	ex_result = tx->do_read(yield); _unused(ex_result);
	if(ex_result != tx_status_t::in_progress || specfac_obj.val_size == 0) {
		tx->abort_rdonly();
		return false;
	}

	/* If we are here, the Special Facility record exists. */
	auto *specfac_val = (tatp_specfac_val_t *) &specfac_obj.val;
	_unused(specfac_val);
	tatp_dassert(specfac_val->data_b[0] == tatp_specfac_data_b0_magic);

	// Lock the Call Forwarding record
	hots_obj_t callfwd_obj;
	tatp_callfwd_key_t callfwd_key;
	callfwd_key.s_id = s_id;
	callfwd_key.sf_type = sf_type;
	callfwd_key.start_time = start_time;
	tx->add_to_write_set(RPC_CALL_FORWARDING_REQ,
		callfwd_key.hots_key, &callfwd_obj, tx_write_mode_t::insert);
	
	ex_result = tx->do_read(yield);
	tatp_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* If callfwd_key existed */

	if(ex_result == tx_status_t::in_progress) {
		/*
		 * callfwd_obj is not a valid Call Forwarding HoTS object yet. We must
		 * construct the real object to insert here.
		 */
		hots_format_real_obj(callfwd_obj, sizeof(tatp_callfwd_val_t));

		tatp_callfwd_val_t *callfwd_val =
			(tatp_callfwd_val_t *) &callfwd_obj.val;
		callfwd_val->numberx[0] = tatp_callfwd_numberx0_magic;
		callfwd_val->end_time = end_time;
		
		tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
		tx_status_t commit_status = tx->commit(yield);
		return (commit_status == tx_status_t::committed);
	} else {
		/*
		 * This happens when the lock_for_ins on the Call Forwarding record
		 * fails. We cannot use abort_rdonly() here, but no RPC requests
		 * will be sent anyway.
		 */
		tx->abort(yield);
		return false;
	}
}

// Profile:
// 1. Read a SECONDARY_SUBSCRIBER row
// 2. Delete a CALL_FORWARDING row
// Maximum messages in a batch = 2 (during commit-backup)
bool txn_delete_call_forwarding(coro_yield_t &yield, int coro_id, Tx *tx)
{
	int txn_type = static_cast<int>(tatp_txn_type_t::delete_call_forwarding);
	tx->start();

	/* Transaction parameters */
	uint32_t s_id = tatp->get_nurand_subscriber(&tg_seed);
	uint8_t sf_type = (hrd_fastrand(&tg_seed) % 4) + 1;
	uint8_t start_time = (hrd_fastrand(&tg_seed) % 3) * 8;

	// Read the secondary subscriber record
	hots_obj_t sec_sub_obj;
	tatp_sec_sub_key_t sec_sub_key;
	sec_sub_key.sub_nbr = tatp->tatp_sub_nbr_from_sid_fast(s_id);

	tx->add_to_read_set(RPC_SEC_SUBSCRIBER_REQ,
		sec_sub_key.hots_key, &sec_sub_obj);
	tx_status_t ex_result = tx->do_read(yield);
	if(ex_result != tx_status_t::in_progress) {
		tx->abort_rdonly();
		return false;
	}

	tatp_dassert(sec_sub_obj.val_size > 0);	/* Must exist */

	tatp_sec_sub_val_t *sec_sub_val = (tatp_sec_sub_val_t *) sec_sub_obj.val;
	_unused(sec_sub_val);
	tatp_dassert(sec_sub_val->magic == tatp_sec_sub_magic);
	tatp_dassert(sec_sub_val->s_id == s_id);

	// Delete the Call Forwarding record if it exists
	hots_obj_t callfwd_obj;
	tatp_callfwd_key_t callfwd_key;
	callfwd_key.s_id = s_id;
	callfwd_key.sf_type = sf_type;
	callfwd_key.start_time = start_time;
	tx->add_to_write_set(RPC_CALL_FORWARDING_REQ,
		callfwd_key.hots_key, &callfwd_obj, tx_write_mode_t::del);
	
	ex_result = tx->do_read(yield);
	tatp_dassert(ex_result == tx_status_t::in_progress ||
		ex_result == tx_status_t::must_abort);	/* If callfwd_key didn't exist */

	if(ex_result == tx_status_t::in_progress) {
		/*
		 * Delete-mode write set records are handled using get_for_upd, so
		 * we have a valid Call Forwarding record here.
		 */
		auto *callfwd_val = (tatp_callfwd_val_t *) &callfwd_obj.val;
		_unused(callfwd_val);
		tatp_dassert(callfwd_val->numberx[0] == tatp_callfwd_numberx0_magic);

		tatp_stat_inc(stat_tx_commit_attempted[txn_type], 1);
		tx_status_t commit_status = tx->commit(yield);
		return (commit_status == tx_status_t::committed);
	} else {
		/*
		 * This happens when the get_for_upd on the Call Forwarding record
		 * fails. We cannot use abort_rdonly() here, but no RPC requests
		 * will be sent anyway.
		 */
		tx->abort(yield);
		return false;
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
#if TATP_COLLECT_STATS == 1
		clock_gettime(CLOCK_REALTIME, &tx_start_time);
#endif
		tatp_txn_type_t txn_type = workgen_arr[hrd_fastrand(&tg_seed) % 100];
		stat_tx_attempted_tot++;
		tatp_stat_inc(stat_tx_attempted[static_cast<int>(txn_type)], 1);

		bool tx_committed = false; /* Did the txn commit? */
		switch(txn_type) {
			case tatp_txn_type_t::get_subsciber_data:
				tx_committed = txn_get_subscriber_data(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::get_new_destination:
				tx_committed = txn_get_new_destination(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::get_access_data:
				tx_committed = txn_get_access_data(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::update_subscriber_data:
				tx_committed = txn_update_subscriber_data(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::update_location:
				tx_committed = txn_update_location(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::insert_call_forwarding:
				tx_committed = txn_insert_call_forwarding(yield, coro_id, tx);
				break;
			case tatp_txn_type_t::delete_call_forwarding:
				tx_committed = txn_delete_call_forwarding(yield, coro_id, tx);
				break;
			default:
				printf("Unexpected transaction type %d\n",
					static_cast<int>(txn_type));
				exit(-1);
		}
		
		if(tx_committed) {
			stat_tx_committed_tot++;
			tatp_stat_inc(stat_tx_committed[static_cast<int>(txn_type)], 1);

#if TATP_COLLECT_STATS == 1
			/* Measure latency of committed txns only */
			clock_gettime(CLOCK_REALTIME, &tx_end_time);
			double tx_usec =
				(tx_end_time.tv_sec - tx_start_time.tv_sec) * 1000000 +
				(double) (tx_end_time.tv_nsec - tx_start_time.tv_nsec) / 1000;
			latency->update(tx_usec * lat_multiplier);
#endif
		}
		
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

#if TATP_COLLECT_STATS == 1
			printf("Worker %d: attempted Tx/s = %.3f M. Commit tput = %.3f M. "
				"Messages/s = {%.1f M, %.1f M coalesced}. "
				"Latency = {%.1f us median, %.1f us 99 percentile}. "
				"{Txn, success rate (expected), commit rate}: "
				"{GSD: %.9f (100), %.3f}, {GND: %.1f (23.9), %.3f}, "
				"{GAD: %.1f (62.5), %.3f}, {USD: %.1f (62.5), %.3f}, "
				"{UL: %.1f (100), %.3f}, {ICF: %.1f (31.25), %.3f}, "
				"{DCF: %.5f (31.25), %.3f}\n",
				wrkr_lid, stat_tx_attempted_tot / msr_usec,
				gs.tx_tput, gs.req_rate, gs.creq_rate,
				(double) latency->perc(.5) / lat_multiplier,
				(double) latency->perc(.99) / lat_multiplier,
				success_pct(tatp_txn_type_t::get_subsciber_data),
				commit_pct(tatp_txn_type_t::get_subsciber_data),
				success_pct(tatp_txn_type_t::get_new_destination),
				commit_pct(tatp_txn_type_t::get_new_destination),
				success_pct(tatp_txn_type_t::get_access_data),
				commit_pct(tatp_txn_type_t::get_access_data),
				success_pct(tatp_txn_type_t::update_subscriber_data),
				commit_pct(tatp_txn_type_t::update_subscriber_data),
				success_pct(tatp_txn_type_t::update_location),
				commit_pct(tatp_txn_type_t::update_location),
				success_pct(tatp_txn_type_t::insert_call_forwarding),
				commit_pct(tatp_txn_type_t::insert_call_forwarding),
				success_pct(tatp_txn_type_t::delete_call_forwarding),
				commit_pct(tatp_txn_type_t::delete_call_forwarding));
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
				0, TATP_TXN_TYPES * sizeof(long long));
			memset((void *) stat_tx_committed,
				0, TATP_TXN_TYPES * sizeof(long long));
			memset((void *) stat_tx_commit_attempted,
				0, TATP_TXN_TYPES * sizeof(long long));
			latency->reset();
		}
	}
}

void parse_config()
{
	// Parse configuration
	auto test_config = ::mica::util::Config::load_file("tatp.json").get("hots");

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
	tatp = params->tatp;
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
	printf("Worker %d: populating TATP tables.\n", wrkr_gid);
	tatp->populate_all_tables_barrier(mappings);
	workgen_arr = tatp->create_workgen_array();
	hrd_red_printf("Worker %d: populated all tables\n", wrkr_gid);

	// Initialize Rpc

	/*
	 * The largest packet will be generated in the UPDATE_SUBSCRIBER_DATA
	 * transaction when both the SUBSCRIBER record and SPECIAL_FACILTY record
	 * are owned by the same remote worker.
	 * During replication, we will send update messages to different machines
	 * so we don't need to worry about coalescing there.
	 */
	int max_pkt_size = (ds_put_req_size(HOTS_MAX_VALUE) +
		sizeof(rpc_cmsg_reqhdr_t)) * 2;

	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		num_workers, workers_per_machine, num_coro,
		base_port_index, num_ports, num_qps, numa_node, postlist, max_pkt_size);
	rpc = new Rpc(_rpc_args);

	tatp->register_rpc_handlers(rpc);

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
