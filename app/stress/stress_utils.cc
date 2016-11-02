#ifndef STRESS_UTILS_H
#define STRESS_UTILS_H

#include "main.h"

/* Called by main. Only initialize here. The worker threads will populate. */
void Stress::init_all_tables()
{
	printf("main: Initializing Stress table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		bool is_primary = (repl_i == 0);
		table[repl_i] = ds_fixedtable_init("table.json",
			sizeof(stress_val_t), TABLE_BASE_SHM_KEY + repl_i, is_primary);
		table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}
}

/*
 * Add a key-value pair into a table during initial population, iff this
 * worker (identified through @mappings) is responsible for populating the
 * key as replica @repl_i.
  *
 * Returns number of inserted keys (0 or 1).
 */
int Stress::load_into_table(Mappings *mappings, FixedTable *table,
	int repl_i, hots_key_t hots_key, void *val_ptr, size_t val_size)
{
	assert(val_size <= HOTS_MAX_VALUE);

	uint64_t keyhash = ds_keyhash(hots_key);
	if(!mappings->should_i_populate(keyhash, repl_i)) {
		return 0;
	}

	/* Construct the object */
	hots_obj_t obj;
	hots_format_real_obj(obj, val_size);
	memcpy((void *) obj.val, val_ptr, val_size);

	/*
	 * There are no coroutines for now, so we can use @wrkr_gid as a
	 * unique caller ID for table functions.
	 */
	uint32_t wrkr_gid = mappings->wrkr_gid;
	MicaResult out_result = table->set_spinlock(wrkr_gid,
		keyhash, hots_key, (char *) obj.val);

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
void Stress::populate_all_tables_barrier(Mappings *mappings)
{
	populate_table(mappings);	/* Only one table */

	__sync_fetch_and_add(&thread_barrier, 1);
	while(thread_barrier != workers_per_machine) {
		sleep(.1);
	}
}

void Stress::populate_table(Mappings *mappings)
{
	printf("Worker %d: Populating table (%d replicas, %u rows)\n",
		mappings->wrkr_gid, mappings->num_replicas, num_rows_total);

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	/* All threads must execute the loop below deterministically */
	uint64_t tmp_seed = 0xdeadbeef;	/* Temporary seed for this function only */
	int tot_records_inserted = 0, tot_records_examined = 0;

	/* Populate the table */
	for(uint64_t key = 0; key < num_rows_total; key++) {
		stress_key_t stress_key;
		stress_val_t stress_val;

		stress_key.key = key;
		stress_val.val = 0;	/* Start with the same value for all objs */

		for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
			/* Insert into table if I am replica number repl_i for this key */
			tot_records_inserted += load_into_table(mappings,
				table[repl_i], repl_i, stress_key.hots_key,
				(void *) &stress_val,
				sizeof(stress_val_t));
			tot_records_examined++;
		}
	}

	clock_gettime(CLOCK_REALTIME, &end);
	double sec = (end.tv_sec - start.tv_sec) +
		(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
	printf("Worker %d: Populated SUBSCRIBER (%d replicas) table in %.1f sec. "
		"Total records inserted = %d, Examined:Inserted = %.2f:1. "
		"Final seed ~ %lu\n",
		mappings->wrkr_gid, mappings->num_replicas, sec, tot_records_inserted,
		(float) tot_records_examined / tot_records_inserted, tmp_seed % 100);
}

#endif /* STRESS_UTILS_H */
