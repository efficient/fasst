// Initialization functions for a FixedTable datastore

#ifndef DS_FIXEDTABLE_H
#define DS_FIXEDTABLE_H

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "libhrd/hrd.h"	// ct_assert and friends
#include "hots.h"
#include "datastore/ds.h"
#include "rpc/rpc.h"
#include "mappings/mappings.h"
#include "util/rte_memcpy.h"

#include "mica/util/config.h"
#include "mica/table/fixedtable.h"
#include "mica/util/hash.h"

#define DS_FIXEDTABLE_DPRINTF 0

// Debug macros
#define ds_fixedtable_printf(fmt, ...) \
	do { \
		if (DS_FIXEDTABLE_DPRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)

/* Use default config with kFetchAddOnlyIfEven = true */
typedef ::mica::table::BasicFixedTableConfig FixedTableConfig;
typedef ::mica::table::FixedTable<FixedTableConfig> FixedTable;
typedef ::mica::table::Result MicaResult;	/* An enum */

// Control path

/*
 * Initialize the table with pre-defined SHM keys.
 * @config_filepath contains config parameters for the allocator and pool.
 * @val_size is the application-level opaque buffer size.
 */
static FixedTable* ds_fixedtable_init(const char *config_filepath,
	size_t val_size, int bkt_shm_key, bool is_primary)
{
	ds_do_checks();

	auto config = ::mica::util::Config::load_file(config_filepath);

	/* Check that CRCW mode is set */
	auto table_config = config.get("table");
	if(!table_config.get("concurrent_read").get_bool() ||
		!table_config.get("concurrent_write").get_bool()) {
		fprintf(stderr,
			"HoTS Error: FixedTable only supports CRCW. Exiting.\n");
		exit(-1);
	}

	FixedTableConfig::Alloc *alloc = new FixedTableConfig::Alloc(
		config.get("alloc"));

	FixedTable *table = new FixedTable(config.get("table"),
		val_size, bkt_shm_key, alloc, is_primary);

	return table;
}

/* Destroy the table */
static void ds_fixedtable_free(FixedTable *table)
{
	delete table;
}

/*
 * Populate the table at worker @wrkr_gid with keys in the range
 * {0, ..., @num_keys - 1}.
 * @val_size is the application-level opaque buffer size.
 *
 * If use_partitions is false, all keys in the range are added to this datastore.
 * In this case, invalid mappings and repl_i should be passed.
 *
 * If use_partitions is set, only keys for which this worker @wrkr_gid's machine
 * is replica @repl_i, and for which @wrkr_gid is the "owner" worker will be
 * populated. This is determined using the should_i_populate() function.
 *
 * Value for key i is a chunk of size val_size with bytes =
 * (a) (i & 0xff) if the const_val argument is not passed
 * (b) (const_val & 0xff) if the const_val argument is passed
 */
static void ds_fixedtable_populate(FixedTable *table,
	size_t num_keys, size_t val_size, Mappings *mappings, int repl_i,
	int wrkr_gid, bool use_partitions, int const_val = -1)
{
	if(!use_partitions) {
		assert(mappings == NULL && repl_i == -1);
	}

	if(repl_i == 0) {
		assert(table->is_primary);
	} else {
		assert(!table->is_primary);
	}

	printf("HoTS: Populating table %s for worker %d as replica %d. "
		"(%lu keys, val size = %lu)\n",
		table->name.c_str(), wrkr_gid, repl_i, num_keys, val_size);

	assert(table != NULL);
	assert(num_keys >= 1);
	assert(val_size == table->val_size);


	/* Initialize common fields for all inserted objects */
	hots_obj_t obj;
	hots_format_real_obj(obj, val_size);

	size_t keys_added = 0;
	
	for(size_t i = 0; i < num_keys; i++) {
		hots_key_t key = (hots_key_t) i;
		uint64_t keyhash = ds_keyhash(key);

		if(use_partitions) {
			assert(repl_i >= 0 && repl_i < HOTS_MAX_REPLICAS);
			if(!mappings->should_i_populate(keyhash, repl_i)) {
				continue;
			}
		}

		keys_added++;
		uint8_t val_byte = (const_val == -1) ? (i & 0xff) : (const_val & 0xff);
		memset((void *) obj.val, val_byte, val_size);

		/*
		 * There are no coroutines for now, so we can use @wrkr_gid as a
		 * unique caller ID for table functions.
		 */
		MicaResult out_result = table->set_spinlock(wrkr_gid,
			keyhash, key, (char *) obj.val);

		if(out_result != MicaResult::kSuccess) {
			fprintf(stderr, "HoTS: Failed to populate table %s for worker %d. "
				"Error at key %" PRIu64 ", code = %s\n",
				table->name.c_str(), wrkr_gid, key,
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}
	}

	printf("HoTS: Done populating table %s for worker %d. Added %lu of %lu keys\n",
		table->name.c_str(), wrkr_gid, keys_added, num_keys);
	fflush(stdout);
}

#include "datastore/fixedtable/ds_fixedtable_handler.h"
#endif 	/* HOTS_MICA */
