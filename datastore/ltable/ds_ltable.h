// Initialization functions for an LTable datastore

#ifndef DS_LTABLE_H
#define DS_LTABLE_H

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
#include "mica/table/ltable.h"
#include "mica/util/hash.h"

typedef ::mica::table::BasicLosslessLTableConfig LTableConfig;
typedef ::mica::table::LTable<LTableConfig> LTable;
typedef ::mica::table::Result MicaResult;	/* An enum */

// Control path

/*
 * Initialize the table with pre-defined SHM keys.
 * @config_filepath contains config parameters for the allocator and pool.
 * @val_size does not include the HoTS object header.
 */
static LTable* ds_ltable_init(const char *config_filepath,
	int bkt_shm_key, int pool_shm_key)
{
	/*
	 * The LTable RPC handler for lock-for-insert is not thread safe: in
	 * concurrent mode, multiple threads can insert a pre-locked object.
	 */
	fprintf(stderr, "LTable currently not supported - need to implement "
		"atomic fetch-and-add in CRCW mode\n");
	exit(-1);

	ds_do_checks();

	auto config = ::mica::util::Config::load_file(config_filepath);

	/* Check that EREW mode is set */
	auto table_config = config.get("table");
	if(table_config.get("concurrent_read").get_bool() ||
		table_config.get("concurrent_write").get_bool()) {
		fprintf(stderr,
			"HoTS Error: LTable only supports EREW. Exiting..\n");
		exit(-1);
	}

	LTableConfig::Alloc *alloc = new LTableConfig::Alloc(config.get("alloc"));
	LTableConfig::Pool *pool = new LTableConfig::Pool(config.get("pool"),
		pool_shm_key, alloc);

	LTable *table = new LTable(config.get("table"), bkt_shm_key, alloc, pool);

	return table;
}

/* Destroy the table */
static void ds_ltable_free(LTable *table)
{
	table->free_pool();	/* We don't have a direct pointer to @pool anymore */
	delete table;
}

/*
 * Populate the table at worker @wrkr_gid with keys in the range
 * {0, ..., @num_keys - 1}.
 * @val_size does not include the HoTS object header.
 *
 * If use_partitions is false, all keys in the range are added to this datastore.
 * In this case, invalid mappings and repl_i should be passed.
 *
 * If use_partitions is set, only keys for which this workers is replica number
 * repl_i are added added to this datastore.
 *
 * Value for key i is a chunk of size val_size with bytes =
 * (a) (i & 0xff) if the const_val argument is not passed
 * (b) (const_val & 0xff) if the const_val argument is passed
 */
static void ds_ltable_populate(LTable *table,
	size_t num_keys, size_t val_size, Mappings *mappings, int repl_i,
	int wrkr_gid, bool use_partitions, int const_val = -1)
{
	if(!use_partitions) {
		assert(mappings == NULL && repl_i == -1);
	}

	printf("HoTS: Populating table %s for worker %d as replica %d. "
		"(%lu keys, val size = %lu)\n",
		table->name.c_str(), wrkr_gid, repl_i, num_keys, val_size);

	assert(table != NULL);
	assert(num_keys >= 1);
	assert(val_size >= 1 && val_size <= HOTS_MAX_VALUE);

	MicaResult out_result;

	/* Initialize common fields for all inserted objects */
	hots_obj_t obj;
	hots_format_real_objhdr(obj, val_size);

	size_t obj_size = hots_obj_size(val_size);
	
	for(size_t i = 0; i < num_keys; i++) {
		hots_key_t key = (hots_key_t) i;
		uint64_t keyhash = ds_keyhash(key);

		if(use_partitions) {
			if(repl_i == 0) {
				/* Skip if wrkr_gid is not the primary partition for this key */
				if(mappings->get_primary_wn(keyhash) != wrkr_gid) {
					continue;
				}
			} else {
				/* Skip if wrkr_gid is not backup #(repl_i - 1) for this key */
				if(mappings->get_backup_wn(keyhash, repl_i - 1) != wrkr_gid) {
					continue;
				}
			}
		}

		uint8_t val_byte = (const_val == -1) ? (i & 0xff) : (const_val & 0xff);
		memset((void *) obj.val, val_byte, val_size);

		out_result = table->set(keyhash, (char *) &key, sizeof(hots_key_t),
			(char *) &obj, obj_size, true);

		if(out_result != MicaResult::kSuccess) {
			fprintf(stderr, "HoTS: Failed to populate table %s for worker %d. "
				"Error at key %" PRIu64 ", code = %s\n",
				table->name.c_str(), wrkr_gid, key,
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}
	}

	printf("HoTS: Done populating table %s for worker %d\n",
		table->name.c_str(), wrkr_gid);
}

#include "datastore/ltable/ds_ltable_handler.h"
#endif 	/* DS_LTABLE_H */
