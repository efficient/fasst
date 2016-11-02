#ifndef SB_H
#define SB_H

#include <vector>

#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "tx/tx.h"
#include "hots.h"
#include "util/rte_memcpy.h"
#include "mappings/mappings.h"
#include "datastore/fixedtable/ds_fixedtable.h"

#include "sb_defs.h"

class SB
{
public:
	volatile int thread_barrier; /* Bumped when a thread finishes populating */
	// Constructor args
	int num_machines;	/* Total machines in cluster */
	int workers_per_machine;	/* For barrier */
	int num_replicas;

	// Derived
	uint32_t tot_primary_workers;
	uint32_t num_accounts_global, num_hot_global;

	/* Tables */
	FixedTable *saving_table[HOTS_MAX_REPLICAS];
	FixedTable *checking_table[HOTS_MAX_REPLICAS];

	SB(int num_machines, int workers_per_machine, int num_replicas) :
		num_machines(num_machines), workers_per_machine(workers_per_machine),
		num_replicas(num_replicas) {
		thread_barrier = 0;
		tot_primary_workers = num_machines * workers_per_machine;

		/* Sanity checks */

		/* Both saving and checking values are 8 bytes in size */
		static_assert(HOTS_MAX_VALUE >= sizeof(uint64_t), "");

		/* Up to 2 billion accounts */
		assert((size_t) tot_primary_workers * DEFAULT_NUM_ACCOUNTS <=
			2ull * 1024 * 1024 * 1024);

		num_accounts_global = tot_primary_workers * DEFAULT_NUM_ACCOUNTS;
		num_hot_global = tot_primary_workers * DEFAULT_NUM_HOT;

		init_all_tables();
	}

	void register_rpc_handlers(Rpc *rpc) const
	{
		assert(rpc != NULL);

		for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
			rpc->register_rpc_handler(RPC_SAVING_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) saving_table[repl_i]);
			rpc->register_rpc_handler(RPC_CHECKING_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) checking_table[repl_i]);
		}
	}

	sb_txn_type_t* create_workgen_array()
	{
		sb_txn_type_t *workgen_arr = new sb_txn_type_t[100];

		int i = 0, j = 0;

		j += FREQUENCY_AMALGAMATE;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::amalgamate;

		j += FREQUENCY_BALANCE;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::balance;

		j += FREQUENCY_DEPOSIT_CHECKING;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::deposit_checking;

		j += FREQUENCY_SEND_PAYMENT;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::send_payment;

		j += FREQUENCY_TRANSACT_SAVINGS;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::transact_saving;

		j += FREQUENCY_WRITE_CHECK;
		for(; i < j; i++) workgen_arr[i] = sb_txn_type_t::write_check;

		assert(i == 100 && j == 100);
		return workgen_arr;
	}

	/*
	 * Generators for new account IDs. Called once per transaction because
 	 * we need to decide hot-or-not per transaction, not per account.
	 */
	inline void get_account(uint64_t *seed, uint64_t *acct_id) const
	{
		if(hrd_fastrand(seed) % 100 < TX_HOT) {
			*acct_id = hrd_fastrand(seed) % num_hot_global;
		} else {
			*acct_id = hrd_fastrand(seed) % num_accounts_global;
		}
	}

	inline void get_two_accounts(uint64_t *seed,
		uint64_t *acct_id_0, uint64_t *acct_id_1) const
	{
		if(hrd_fastrand(seed) % 100 < TX_HOT) {
			*acct_id_0 = hrd_fastrand(seed) % num_hot_global;
			*acct_id_1 = hrd_fastrand(seed) % num_hot_global;
			while(*acct_id_1 == *acct_id_0) {
				*acct_id_1 = hrd_fastrand(seed) % num_hot_global;
			}
		} else {
			*acct_id_0 = hrd_fastrand(seed) % num_accounts_global;
			*acct_id_1 = hrd_fastrand(seed) % num_accounts_global;
			while(*acct_id_1 == *acct_id_0) {
				*acct_id_1 = hrd_fastrand(seed) % num_accounts_global;
			}
		}
	}

	/* sb_utils.cpp */
	void init_all_tables();
	void populate_all_tables_barrier(Mappings *mappings);
	void populate_savings_and_checking_table(Mappings *mappings);
	int load_into_table(Mappings *mappings, FixedTable *table,
		int repl_i, hots_key_t hots_key, void *val_ptr, size_t val_size);

};

#endif
