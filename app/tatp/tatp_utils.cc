#ifndef TATP_UTILS_H
#define TATP_UTILS_H

#include "main.h"

/* Called by main. Only initialize here. The worker threads will populate. */
void TATP::init_all_tables()
{
	printf("main: Initializing SUBSCRIBER table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		subscriber_table[repl_i] = ds_fixedtable_init(
			"tatp_json/subscriber.json", sizeof(tatp_sub_val_t),
			SUBSCRIBER_BASE_SHM_KEY + repl_i, repl_i == 0);
		subscriber_table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}

	printf("main: Initializing SECONDARY SUBSCRIBER table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		sec_subscriber_table[repl_i] = ds_fixedtable_init(
			"tatp_json/secondary_subscriber.json", sizeof(tatp_sec_sub_val_t),
			SEC_SUBSCRIBER_BASE_SHM_KEY + repl_i, repl_i == 0);
		sec_subscriber_table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}

	printf("main: Initializing ACCESS INFO table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		access_info_table[repl_i] = ds_fixedtable_init(
			"tatp_json/access_info.json", sizeof(tatp_accinf_val_t),
			ACCESS_INFO_BASE_SHM_KEY + repl_i, repl_i == 0);
		access_info_table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}

	printf("main: Initializing SPECIAL FACILITY table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		special_facility_table[repl_i] = ds_fixedtable_init(
			"tatp_json/special_facility.json", sizeof(tatp_specfac_val_t),
			SPECIAL_FACILTY_BASE_SHM_KEY + repl_i, repl_i == 0);
		special_facility_table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}

	printf("main: Initializing CALL FORWARDING table\n");
	for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
		call_forwarding_table[repl_i] = ds_fixedtable_init(
			"tatp_json/call_forwarding.json", sizeof(tatp_callfwd_val_t),
			CALL_FORWARDING_BASE_SHM_KEY + repl_i, repl_i == 0);
		call_forwarding_table[repl_i]->name.append("-replica-" +
			std::to_string(repl_i));
	}

	fflush(stdout);
}

/*
 * Add a key-value pair into a table during initial population, iff this
 * worker (identified through @mappings) is responsible for populating the
 * key as replica @repl_i.
  *
 * Returns number of inserted keys (0 or 1).
 */
int TATP::load_into_table(Mappings *mappings, FixedTable *table,
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
 * Select between N and M unique items from the values vector. The number
 * of values to be selected, and the actual values are chosen at random.
 */
std::vector<uint8_t> TATP::select_between_n_and_m_from(uint64_t *tmp_seed,
	std::vector<uint8_t> values, unsigned N, unsigned M)
{
	assert(M >= N);
	assert(M >= values.size());

	std::vector<uint8_t> ret;

	int used[32];
	memset(used, 0, 32 * sizeof(int));

	int to_select = (hrd_fastrand(tmp_seed) % (M - N + 1)) + N;
	for(int i = 0; i < to_select; i++) {
		int index = hrd_fastrand(tmp_seed) % values.size();
		uint8_t value = values[index];
		assert(value < 32);

		if(used[value] == 1) {
			i--;
			continue;
		}

		used[value] = 1;
		ret.push_back(value);
	}

	return ret;
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
void TATP::populate_all_tables_barrier(Mappings *mappings)
{
	populate_subscriber_table(mappings);
	populate_secondary_subscriber_table(mappings);
	populate_access_info_table(mappings);
	populate_specfac_and_callfwd_table(mappings);

	__sync_fetch_and_add(&thread_barrier, 1);
	while(thread_barrier != workers_per_machine) {
		sleep(.1);
	}
}

void TATP::populate_subscriber_table(Mappings *mappings)
{
	printf("Worker %d: Populating SUBSCRIBER table (%d replicas, %u subs)\n",
		mappings->wrkr_gid, mappings->num_replicas, subscriber_size);

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	/* All threads must execute the loop below deterministically */
	uint64_t tmp_seed = 0xdeadbeef;	/* Temporary seed for this function only */
	int tot_records_inserted = 0, tot_records_examined = 0;

	/* Populate the table */
	for(uint32_t s_id = 0; s_id < subscriber_size; s_id++) {
		tatp_sub_key_t key;
		key.s_id = s_id;
		
		/* Initialize the subscriber payload */
		tatp_sub_val_t sub_val;
		sub_val.sub_nbr = tatp_sub_nbr_from_sid(s_id);

		for(int i = 0; i < 5; i++) {
			sub_val.hex[i] = hrd_fastrand(&tmp_seed);
		}

		for(int i = 0; i < 10; i++) {
			sub_val.bytes[i] = hrd_fastrand(&tmp_seed);
		}

		sub_val.bits = hrd_fastrand(&tmp_seed);
		sub_val.msc_location = tatp_sub_msc_location_magic;	/* Debug */
		sub_val.vlr_location = hrd_fastrand(&tmp_seed);

		for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
			/* Insert into table if I am replica number repl_i for this key */
			tot_records_inserted += load_into_table(mappings,
				subscriber_table[repl_i], repl_i,
				key.hots_key, (void *) &sub_val, sizeof(tatp_sub_val_t));
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
	fflush(stdout);
}

void TATP::populate_secondary_subscriber_table(Mappings *mappings)
{
	printf("Worker %d: Populating secondary SUBSCRIBER table (%d replicas)\n",
		mappings->wrkr_gid, mappings->num_replicas);

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int tot_records_inserted = 0, tot_records_examined = 0;

	/* Populate the tables */
	for(uint32_t s_id = 0; s_id < subscriber_size; s_id++) {
		tatp_sec_sub_key_t key;
		key.sub_nbr = tatp_sub_nbr_from_sid(s_id);
		
		/* Initialize the subscriber payload */
		tatp_sec_sub_val_t sec_sub_val;
		sec_sub_val.s_id = s_id;
		sec_sub_val.magic = tatp_sec_sub_magic;

		for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
			/* Insert into table if I am replica number repl_i for this key */
			tot_records_inserted += load_into_table(mappings,
				sec_subscriber_table[repl_i], repl_i, key.hots_key,
				(void *) &sec_sub_val, sizeof(tatp_sec_sub_val_t));
			tot_records_examined++;
		}
	}

	clock_gettime(CLOCK_REALTIME, &end);
	double sec = (end.tv_sec - start.tv_sec) +
		(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
	printf("Worker %d: Populated secondary SUBSCRIBER table (%d replicas) "
		"in %.1f sec. Total records inserted = %d, "
		"Examined:Inserted = %.2f:1.\n",
		mappings->wrkr_gid, mappings->num_replicas, sec, tot_records_inserted,
		(float) tot_records_examined / tot_records_inserted);
	fflush(stdout);
}

void TATP::populate_access_info_table(Mappings *mappings)
{
	printf("Worker %d: Initializing ACCESS INFO table (%d replicas)\n",
		mappings->wrkr_gid, mappings->num_replicas);

	std::vector<uint8_t> ai_type_values = {1, 2, 3, 4};

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	/* All threads must execute the loop below deterministically */
	uint64_t tmp_seed = 0xdeadbeef;	/* Temporary seed for this function only */
	int tot_records_inserted = 0, tot_records_examined = 0;

	/* Populate the table */
	for(uint32_t s_id = 0; s_id < subscriber_size; s_id++) {
		std::vector<uint8_t> ai_type_vec = select_between_n_and_m_from(
			&tmp_seed, ai_type_values, 1, 4);

		for(uint8_t ai_type : ai_type_vec) {
			/* Insert access info record */
			tatp_accinf_key_t key;
			key.s_id = s_id;
			key.ai_type = ai_type;
			
			tatp_accinf_val_t accinf_val;
			accinf_val.data1 = tatp_accinf_data1_magic;

			for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
				/* Insert into table if I am replica number repl_i for key */
				tot_records_inserted += load_into_table(mappings,
					access_info_table[repl_i], repl_i, key.hots_key,
					(void *) &accinf_val, sizeof(tatp_accinf_val_t));
				tot_records_examined++;
			}
		}
	}

	clock_gettime(CLOCK_REALTIME, &end);
	double sec = (end.tv_sec - start.tv_sec) +
		(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
	printf("Worker %d: Populated ACCESS INFO table (%d replicas) in %.1f sec. "
		"Total records inserted = %d, Examined:Inserted = %.2f:1. "
		"Final seed ~ %lu\n",
		mappings->wrkr_gid, mappings->num_replicas, sec, tot_records_inserted,
		(float) tot_records_examined / tot_records_inserted, tmp_seed % 100);
	fflush(stdout);
}

/*
 * Which rows are inserted into the CALL FORWARDING table depends on which
 * rows get inserted into the SPECIAL FACILTY, so process these two jointly.
 */
void TATP::populate_specfac_and_callfwd_table(Mappings *mappings)
{
	printf("Worker %d: Populating SPECIAL FACILITY and CALL FORWARDING table "
		"(%d replicas)\n", mappings->wrkr_gid, mappings->num_replicas);

	std::vector<uint8_t> sf_type_values = {1, 2, 3, 4};
	std::vector<uint8_t> start_time_values = {0, 8, 16};

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);
	int tot_records_inserted = 0, tot_records_examined = 0;

	/* All threads must execute the loop below deterministically */
	uint64_t tmp_seed = 0xdeadbeef;	/* Temporary seed for this function only */

	/* Populate the tables */
	for(uint32_t s_id = 0; s_id < subscriber_size; s_id++) {
		std::vector<uint8_t> sf_type_vec = select_between_n_and_m_from(
			&tmp_seed, sf_type_values, 1, 4);

		for(uint8_t sf_type : sf_type_vec) {
			/* Insert the special facility record */
			tatp_specfac_key_t key;
			key.s_id = s_id;
			key.sf_type = sf_type;
			
			tatp_specfac_val_t specfac_val;
			specfac_val.data_b[0] = tatp_specfac_data_b0_magic;
			specfac_val.is_active = (hrd_fastrand(&tmp_seed) % 100 < 85) ? 1 : 0;

			for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
				/* Insert into table if I am replica number repl_i for key */
				tot_records_inserted += load_into_table(mappings,
					special_facility_table[repl_i], repl_i, key.hots_key,
					(void *) &specfac_val, sizeof(tatp_specfac_val_t));
				tot_records_examined++;
			}

			/*
			 * The TATP spec requires a different initial probability
			 * distribution of Call Forwarding records (see README). Here, we
			 * populate the table using the steady state distribution.
			 */
			for(size_t start_time = 0; start_time <= 16; start_time += 8) {
				/*
				 * At steady state, each @start_time for <s_id, sf_type> is
				 * equally likely to be present or absent.
				 */
				if(hrd_fastrand(&tmp_seed) % 2 == 0) {
					continue;
				}

				/* Insert the call forwarding record */
				tatp_callfwd_key_t key;
				key.s_id = s_id;
				key.sf_type = sf_type;
				key.start_time = start_time;
				
				tatp_callfwd_val_t callfwd_val;
				callfwd_val.numberx[0] = tatp_callfwd_numberx0_magic;
				/* At steady state, @end_time is unrelated to @start_time */
				callfwd_val.end_time = (hrd_fastrand(&tmp_seed) % 24) + 1;

				for(int repl_i = 0; repl_i < mappings->num_replicas; repl_i++) {
					/* Insert into table if I am replica number repl_i for key */
					tot_records_inserted += load_into_table(mappings,
						call_forwarding_table[repl_i], repl_i, key.hots_key,
						(void *) &callfwd_val, sizeof(tatp_callfwd_val_t));
					tot_records_examined++;
				}

			}	/* End loop start_time */
		}	/* End loop sf_type */
	}	/* End loop s_id */

	clock_gettime(CLOCK_REALTIME, &end);
	double sec = (end.tv_sec - start.tv_sec) +
		(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
	printf("Worker %d: Populated SPECIAL FACILITY and CALL FORWARDING table "
		"(%d replicas) in %.1f seconds. " 
		"Total records inserted = %d, Examined:Inserted = %.2f:1. "
		"Final seed ~ %lu\n",
		mappings->wrkr_gid, mappings->num_replicas, sec, tot_records_inserted,
		(float) tot_records_examined / tot_records_inserted, tmp_seed % 100);
	fflush(stdout);
}

/*
 * Test fast SID to subscriber number string generation. This takes a while
 * so it should be run alone (it calls exit() on completion).
 */
void TATP::tatp_test_map_1000()
{
	printf("TATP: Testing map_1000\n");

	/* Exhaustively test all s_id's */
	for(uint32_t s_id = 0; s_id < TATP_MAX_SUBSCRIBERS; s_id++) {
		if(s_id % 1000000 == 0) {
			printf("TATP: Tested up to s_id = %u\n", s_id);
		}

		/* Compare simple algorithm (a) to fast algorithm (b) */
		tatp_sub_nbr_t sub_nbr_a = tatp_sub_nbr_from_sid(s_id);
		tatp_sub_nbr_t sub_nbr_b = tatp_sub_nbr_from_sid_fast(s_id);

		if(sub_nbr_a.hots_key != sub_nbr_b.hots_key) {
			printf("TATP: map_1000 error at s_id = %u\n", s_id);
			exit(-1);
		}
	}

	/* Compare the speed of the two algorithms */
	uint64_t sum = 0;
	struct timespec start, end;

	/* Simple algorithm (one digit per modulo) */
	clock_gettime(CLOCK_REALTIME, &start);
	for(uint32_t s_id = 0; s_id < TATP_MAX_SUBSCRIBERS; s_id++) {
		tatp_sub_nbr_t sub_nbr_a = tatp_sub_nbr_from_sid(s_id);
		sum += sub_nbr_a.hots_key;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	double nsec = (end.tv_sec - start.tv_sec) * 1000000000 + 
		(double) (end.tv_nsec - start.tv_nsec);
	printf("TATP: Simple method: sum = %llu, time = %.3f ns per call\n",
		(unsigned long long) sum, nsec / TATP_MAX_SUBSCRIBERS);

	/* Fast algorithm (3 digits per modulo and map lookup) */
	sum = 0;
	clock_gettime(CLOCK_REALTIME, &start);
	for(uint32_t s_id = 0; s_id < TATP_MAX_SUBSCRIBERS; s_id++) {
		tatp_sub_nbr_t sub_nbr_b = tatp_sub_nbr_from_sid_fast(s_id);
		sum += sub_nbr_b.hots_key;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	nsec = (end.tv_sec - start.tv_sec) * 1000000000 + 
		(double) (end.tv_nsec - start.tv_nsec);
	printf("TATP: Fast method: sum = %llu, time = %.3f ns per call\n",
		(unsigned long long) sum, nsec / TATP_MAX_SUBSCRIBERS);
	
	printf("TATP: map_1000 test successful. Exiting!\n");
	exit(-1);
}

#endif /* TATP_UTILS_H */
