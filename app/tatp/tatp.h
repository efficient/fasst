#ifndef TATP_H
#define TATP_H

#include <vector>

#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "tx/tx.h"
#include "hots.h"
#include "util/rte_memcpy.h"
#include "mappings/mappings.h"
#include "datastore/fixedtable/ds_fixedtable.h"

#include "tatp_defs.h"
#include "tatp_string.h"

class TATP
{
public:
	volatile int thread_barrier; /* Bumped when a thread finishes populating */
	uint16_t *map_1000; /* Map 0--999 to 12b, 4b/digit decimal representation */

	// Constructor args
	int num_machines;	/* Total machines in cluster */
	int workers_per_machine;	/* For barrier */
	int num_replicas;

	// Derived
	uint32_t subscriber_size;
	uint32_t A;	/* TATP spec parameter for non-uniform random generation */

	/* Tables */
	FixedTable *subscriber_table[HOTS_MAX_REPLICAS];
	FixedTable *sec_subscriber_table[HOTS_MAX_REPLICAS];
	FixedTable *special_facility_table[HOTS_MAX_REPLICAS];
	FixedTable *access_info_table[HOTS_MAX_REPLICAS];
	FixedTable *call_forwarding_table[HOTS_MAX_REPLICAS];

	TATP(int num_machines, int workers_per_machine, int num_replicas) :
		num_machines(num_machines), workers_per_machine(workers_per_machine),
		num_replicas(num_replicas) {
		thread_barrier = 0;

		/* Init the precomputed decimal map */
		map_1000 = (uint16_t *) malloc(1000 * sizeof(uint16_t));
		for(size_t i = 0; i < 1000; i++) {
			uint32_t dig_1 = (i / 1) % 10;
			uint32_t dig_2 = (i / 10) % 10;
			uint32_t dig_3 = (i / 100) % 10;
			map_1000[i] = (dig_3 << 8) | (dig_2 << 4) | dig_1;
		}
		//tatp_test_map_1000();	/* Enable this to exhaustively test the map */

		/*
		 * Ensure that max MICA value size is exactly the largest of the
		 * datastore value sizes.
		 * This allows a simpler computation of max_pkt_size for RPC. It also
		 * keeps max_pkt_size small, which improves performance a bit.
		 */
		static_assert(HOTS_MAX_VALUE == 40, "");

		assert((size_t) num_machines * SUBSCRIBERS_PER_MACHINE
			<= TATP_MAX_SUBSCRIBERS);
		subscriber_size = num_machines * SUBSCRIBERS_PER_MACHINE;

		/* Compute the "A" parameter for nurand distribution as per spec */
		if(subscriber_size <= 1000000) {
			A = 65535;
		} else if(subscriber_size <= 10000000) {
			A = 1048575;
		} else {
			A = 2097151;
		}

		init_all_tables();
	}

	void register_rpc_handlers(Rpc *rpc) const
	{
		assert(rpc != NULL);

		for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
			rpc->register_rpc_handler(RPC_SUBSCRIBER_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) subscriber_table[repl_i]);
			rpc->register_rpc_handler(RPC_SEC_SUBSCRIBER_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) sec_subscriber_table[repl_i]);
			rpc->register_rpc_handler(RPC_SPECIAL_FACILITY_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) special_facility_table[repl_i]);
			rpc->register_rpc_handler(RPC_ACCESS_INFO_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) access_info_table[repl_i]);
			rpc->register_rpc_handler(RPC_CALL_FORWARDING_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) call_forwarding_table[repl_i]);
		}
	}

	
	tatp_txn_type_t* create_workgen_array()
	{
		tatp_txn_type_t *workgen_arr = new tatp_txn_type_t[100];

		int i = 0, j = 0;

		j += FREQUENCY_GET_SUBSCRIBER_DATA;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::get_subsciber_data;

		j += FREQUENCY_GET_ACCESS_DATA;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::get_access_data;

		j += FREQUENCY_GET_NEW_DESTINATION;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::get_new_destination;

		j += FREQUENCY_UPDATE_SUBSCRIBER_DATA;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::update_subscriber_data;

		j += FREQUENCY_UPDATE_LOCATION;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::update_location;

		j += FREQUENCY_INSERT_CALL_FORWARDING;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::insert_call_forwarding;

		j += FREQUENCY_DELETE_CALL_FORWARDING;
		for(; i < j; i++) workgen_arr[i] = tatp_txn_type_t::delete_call_forwarding;

		assert(i == 100 && j == 100);
		return workgen_arr;
	}

	/*
	 * Get a non-uniform-random distributed subscriber ID according to spec.
	 * To get a non-uniformly random number between 0 and y:
	 * NURand(A, 0, y) = (get_random(0, A) | get_random(0, y)) % (y + 1)
	 */
	forceinline uint32_t get_nurand_subscriber(uint64_t *tg_seed) const
	{
		return ((hrd_fastrand(tg_seed) % subscriber_size) |
			(hrd_fastrand(tg_seed) & A)) % subscriber_size;
	}

	/* Get a subscriber number from a subscriber ID, fast */
	forceinline tatp_sub_nbr_t tatp_sub_nbr_from_sid_fast(uint32_t s_id)
	{
		tatp_sub_nbr_t sub_nbr;
		sub_nbr.hots_key = 0;
		sub_nbr.dec_0_1_2 = map_1000[s_id % 1000];
		s_id /= 1000;
		sub_nbr.dec_3_4_5 = map_1000[s_id % 1000];
		s_id /= 1000;
		sub_nbr.dec_6_7_8 = map_1000[s_id % 1000];

		return sub_nbr;
	}

	/* Get a subscriber number from a subscriber ID, simple */
	forceinline tatp_sub_nbr_t tatp_sub_nbr_from_sid(uint32_t s_id)
	{
		#define update_sid() \
			do { \
				s_id = s_id / 10; \
				if(s_id == 0) { \
					return sub_nbr; \
				} \
			} while(false)

		tatp_sub_nbr_t sub_nbr;
		sub_nbr.hots_key = 0;	/* Zero out all digits */

		sub_nbr.dec_0 = s_id % 10;
		update_sid();

		sub_nbr.dec_1 = s_id % 10;
		update_sid();

		sub_nbr.dec_2 = s_id % 10;
		update_sid();

		sub_nbr.dec_3 = s_id % 10;
		update_sid();

		sub_nbr.dec_4 = s_id % 10;
		update_sid();

		sub_nbr.dec_5 = s_id % 10;
		update_sid();

		sub_nbr.dec_6 = s_id % 10;
		update_sid();

		sub_nbr.dec_7 = s_id % 10;
		update_sid();

		sub_nbr.dec_8 = s_id % 10;
		update_sid();

		sub_nbr.dec_9 = s_id % 10;
		update_sid();

		sub_nbr.dec_10 = s_id % 10;
		update_sid();

		assert(s_id == 0);
		return sub_nbr;
	}

	

	/* tatp_utils.cpp */
	void init_all_tables();
	void populate_all_tables_barrier(Mappings *mappings);

	static std::vector<uint8_t> select_between_n_and_m_from(uint64_t *tmp_seed,
		std::vector<uint8_t> values, unsigned N, unsigned M);

	int load_into_table(Mappings *mappings, FixedTable *table,
		int repl_i, hots_key_t hots_key, void *val_ptr, size_t val_size);

	void populate_subscriber_table(Mappings *mappings);
	void populate_secondary_subscriber_table(Mappings *mappings);
	void populate_access_info_table(Mappings *mappings);
	void populate_specfac_and_callfwd_table(Mappings *mappings);
	void tatp_test_map_1000();
};
#endif
