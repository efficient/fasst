#ifndef STRESS_H
#define STRESS_H

#include <vector>

#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "tx/tx.h"
#include "hots.h"
#include "util/rte_memcpy.h"
#include "mappings/mappings.h"
#include "datastore/fixedtable/ds_fixedtable.h"

#include "stress_defs.h"

class Stress
{
public:
	volatile int thread_barrier; /* Bumped when a thread finishes populating */
	// Constructor args
	int num_machines;	/* Total machines in cluster */
	int workers_per_machine;	/* For barrier */
	int num_replicas;

	// Derived
	uint32_t num_rows_total;

	/* Tables */
	FixedTable *table[HOTS_MAX_REPLICAS];

	Stress(int num_machines, int workers_per_machine, int num_replicas) :
		num_machines(num_machines), workers_per_machine(workers_per_machine),
		num_replicas(num_replicas) {
		thread_barrier = 0;

		static_assert(HOTS_MAX_VALUE >= sizeof(stress_val_t), "");
		num_rows_total = num_machines * ROWS_PER_MACHINE;

		init_all_tables();
	}

	void register_rpc_handlers(Rpc *rpc) const
	{
		assert(rpc != NULL);

		for(int repl_i = 0; repl_i < num_replicas; repl_i++) {
			rpc->register_rpc_handler(RPC_STRESS_TABLE_REQ + repl_i,
				ds_fixedtable_rpc_handler, (void *) table[repl_i]);
		}
	}

	stress_txn_type_t* create_workgen_array()
	{
		auto *workgen_arr = new stress_txn_type_t[100];

		int i = 0, j = 0;

		j += FREQUENCY_GET_N;
		for(; i < j; i++) workgen_arr[i] = stress_txn_type_t::get_N;

		j += FREQUENCY_DEL_N;
		for(; i < j; i++) workgen_arr[i] = stress_txn_type_t::del_N;

		j += FREQUENCY_INS_N;
		for(; i < j; i++) workgen_arr[i] = stress_txn_type_t::ins_N;

		assert(i == 100 && j == 100);
		return workgen_arr;
	}

	/* stress_utils.cpp */
	void init_all_tables();
	void populate_all_tables_barrier(Mappings *mappings);

	int load_into_table(Mappings *mappings, FixedTable *table,
		int repl_i, hots_key_t hots_key, void *val_ptr, size_t val_size);

	void populate_table(Mappings *mappings);
};
#endif
