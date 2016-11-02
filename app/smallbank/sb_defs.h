#ifndef SB_DEFS
#define SB_DEFS

#include "assert.h"

// Debug macros
#define SB_DEBUG_ASSERT 1

/*
 * Collect stats for
 * a. Total commit rate
 * b. Commit rate for each transaction type
 * c. Latency (median and 99 percentile)
 */
#define SB_COLLECT_STATS 1

#define sb_dassert(x) \
	do { if (SB_DEBUG_ASSERT) assert(x); } while (0)
#define sb_stat_inc(x, y) \
	do {if (SB_COLLECT_STATS) x += y;} while (0)

/* STORED PROCEDURE EXECUTION FREQUENCIES (0-100) */
#define FREQUENCY_AMALGAMATE        15
#define FREQUENCY_BALANCE			15
#define FREQUENCY_DEPOSIT_CHECKING	15
#define FREQUENCY_SEND_PAYMENT		25
#define FREQUENCY_TRANSACT_SAVINGS	15
#define FREQUENCY_WRITE_CHECK		15

/* Taken from DrTM */
#define DEFAULT_NUM_ACCOUNTS 100000	 /* Accounts per partition */
#define DEFAULT_NUM_HOT 4000	/* Hot accounts per partition */
#define TX_HOT 90	/* Percentage of txns that use accounts from hotspot */

/* SHM keys for tables: (f + 1) keys per table at every machine */
#define SAVING_BASE_SHM_KEY 2000
#define CHECKING_BASE_SHM_KEY 4000
#define SB_MAX_SHM_KEY 6000

// Smallbank table keys and values
// All keys have been sized to 8 bytes
// All values have been sized to the next multiple of 8 bytes

/*
 * SAVINGS table.
 */
union sb_sav_key_t {
	uint64_t acct_id;
	hots_key_t hots_key;

	sb_sav_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(sb_sav_key_t) == sizeof(hots_key_t), "");

struct sb_sav_val_t {
	uint32_t magic;
	float bal;
};
static_assert(sizeof(sb_sav_val_t) == sizeof(uint64_t), "");

/*
 * CHECKING table
 */
union sb_chk_key_t {
	uint64_t acct_id;
	hots_key_t hots_key;

	sb_chk_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(sb_chk_key_t) == sizeof(hots_key_t), "");

struct sb_chk_val_t {
	uint32_t magic;
	float bal;
};
static_assert(sizeof(sb_chk_val_t) == sizeof(uint64_t), "");

// Magic numbers for debugging. These are unused in the spec.
#define SB_MAGIC 97	/* Some magic number <= 255 */
#define sb_sav_magic (SB_MAGIC)
#define sb_chk_magic (SB_MAGIC + 1)

// Helpers for generating workload
#define SB_TXN_TYPES 6
enum class sb_txn_type_t : int {
	amalgamate,
	balance,
	deposit_checking,
	send_payment,
	transact_saving,
	write_check,
};

#endif /* SB_DEFS */
