#ifndef SB_UTILS_H
#define SB_UTILS_H

#include <stdint.h>
#include <stdlib.h>

#include "main.h"

/* Called by main. Only initialize here. The worker threads will populate. */
void SB::init_all_tables()
{
	printf("main: Initializing SmallBank tables (%d replicas)\n", num_replicas);

	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		saving_table[repl_i] = ds_fixedtable_init("sb_json/saving.json",
			sizeof(sb_sav_val_t), SAVING_BASE_SHM_KEY + repl_i, repl_i == 0);
	}

	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		checking_table[repl_i] = ds_fixedtable_init("sb_json/checking.json",
			sizeof(sb_chk_val_t), CHECKING_BASE_SHM_KEY + repl_i, repl_i == 0);
	}
}

/*
 * Add a key-value pair into a table during initial population, iff this
 * worker (identified through @mappings) is responsible for populating the
 * key as replica @repl_i.
  *
 * Returns number of inserted keys (0 or 1).
 */
int SB::load_into_table(Mappings *mappings, FixedTable *table,
	int repl_i, hots_key_t hots_key, void *val_ptr, size_t val_size)
{
	assert(val_size <= HOTS_MAX_VALUE);
	assert(val_size == table->val_size);

	uint64_t keyhash = ds_keyhash(hots_key);
	if(!mappings->should_i_populate(keyhash, repl_i)) {
		return 0;
	}

	/* Insert into MICA */
	/*
	 * There are no coroutines for now, so we can use @wrkr_gid as a unique
	 * caller ID for table functions.
	 */
	MicaResult out_result = table->set_spinlock(mappings->wrkr_gid,
		keyhash, hots_key, (const char *) val_ptr);

	if(out_result != MicaResult::kSuccess) {
		fprintf(stderr, "HoTS: Failed to populate table %s "
			"for worker %d. Error at key %" PRIu64 ". Error = %s\n",
			table->name.c_str(), mappings->wrkr_gid, hots_key,
			::mica::table::ResultString(out_result).c_str());

		table->print_bucket_occupancy();
		exit(-1);
	}

	return 1;
}

/*
 * Called by every worker thread.
 *
 * The worker threads on a machine divide the task of populating the machine's
 * partition amongst them. A worker thread must not create an Rpc end point
 * until the machine's partition is fully populated, which happens when it
 * crosses the barrier below.
 *
 * Without the barrier, we may have a case where (a) thread #0 on every machine
 * finishes populating keys that it "owns", (b) these threads establish Rpc
 * end points by exchanging QP info, (c) these threads issue transactions to
 * each other, (d) thread #1 on some machine has still not finished populating
 * its share.
 */
void SB::populate_all_tables_barrier(Mappings *mappings)
{
	populate_savings_and_checking_table(mappings);

	__sync_fetch_and_add(&thread_barrier, 1);
	while(thread_barrier != workers_per_machine) {
		sleep(.1);
	}
}

void SB::populate_savings_and_checking_table(Mappings *mappings)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	/* Populate the tables */
	for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
		for(uint32_t acct_id = 0; acct_id < num_accounts_global; acct_id++) {
			// Savings
			sb_sav_key_t sav_key;
			sav_key.acct_id = (uint64_t) acct_id;

			sb_sav_val_t sav_val;
			sav_val.magic = sb_sav_magic;
			sav_val.bal = 1000000000ull;
			
			/* Inserts into table if I am replica number @repl_i for this key */
			load_into_table(mappings, saving_table[repl_i], repl_i,
				sav_key.hots_key, (void *) &sav_val, sizeof(sb_sav_val_t));


			// Checking
			sb_chk_key_t chk_key;
			chk_key.acct_id = (uint64_t) acct_id;

			sb_chk_val_t chk_val;
			chk_val.magic = sb_chk_magic;
			chk_val.bal = 1000000000ull;
			
			/* Inserts into table if I am replica number @repl_i for this key */
			load_into_table(mappings, checking_table[repl_i], repl_i,
				chk_key.hots_key, (void *) &chk_val, sizeof(sb_chk_val_t));
		}
	}

	clock_gettime(CLOCK_REALTIME, &end);
	double sec = (end.tv_sec - start.tv_sec) +
		(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
	printf("Worker %d: Populated SAVING and CHECKING table in %.1f seconds\n",
		mappings->wrkr_gid, sec);

}

#endif /* SB_UTILS_H */
