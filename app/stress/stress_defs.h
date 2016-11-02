#ifndef STRESS_DEFS
#define STRESS_DEFS

#include "assert.h"

// Debug macros
#define STRESS_DEBUG_ASSERT 1
#define STRESS_DEBUG_PRINTF 0
#define STRESS_COLLECT_STATS 1

#define stress_dprintf(fmt, ...) \
	do { \
		if (STRESS_DEBUG_PRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)

#define stress_dassert(x) \
	do { if (STRESS_DEBUG_ASSERT) assert(x); } while (0)
#define stress_stat_inc(x, y) \
	do {if (STRESS_COLLECT_STATS) x += y;} while (0)

/* STORED PROCEDURE EXECUTION FREQUENCIES (0-100) */
#define FREQUENCY_GET_N 30
#define FREQUENCY_DEL_N 35 
#define FREQUENCY_INS_N 35

#define ROWS_PER_MACHINE 100

/* SHM keys for tables: (f + 1) keys per table at every machine */
#define TABLE_BASE_SHM_KEY 2000

union stress_key_t {
	uint64_t key;
	hots_key_t hots_key;

	stress_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(stress_key_t) == sizeof(hots_key_t), "");

struct stress_val_t {
	uint64_t val;
};
static_assert(sizeof(stress_val_t) == sizeof(uint64_t), "");

// Helpers for generating workload
#define STRESS_TXN_TYPES 3
enum class stress_txn_type_t : int {
	get_N,
	del_N,
	ins_N
};

#endif /* STRESS_DEFS */
