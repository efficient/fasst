#ifndef TATP_DEFS
#define TATP_DEFS

#include "assert.h"
#include "tatp_string.h"

/*
 * Up to 1 billion subscribers so that tatp_sub_nbr_from_sid_fast() requires
 * only 3 modulo operations.
 */
#define TATP_MAX_SUBSCRIBERS 1000000000

// Debug macros
#define TATP_DEBUG_ASSERT 1

/*
 * Collect stats for
 * a. Total commit rate
 * b. Commit rate for each transaction type
 * c. Latency (median and 99 percentile)
 */
#define TATP_COLLECT_STATS 1

#define tatp_dassert(x) \
	do { if (TATP_DEBUG_ASSERT) assert(x); } while (0)
#define tatp_stat_inc(x, y) \
	do {if (TATP_COLLECT_STATS) x += y;} while (0)

/* STORED PROCEDURE EXECUTION FREQUENCIES (0-100) */
#define FREQUENCY_GET_SUBSCRIBER_DATA        35   // Single
#define FREQUENCY_GET_ACCESS_DATA            35   // Single
#define FREQUENCY_GET_NEW_DESTINATION        10   // Single
#define FREQUENCY_UPDATE_SUBSCRIBER_DATA     2    // Single
#define FREQUENCY_UPDATE_LOCATION            14   // Multi
#define FREQUENCY_INSERT_CALL_FORWARDING     2    // Multi
#define FREQUENCY_DELETE_CALL_FORWARDING     2    // Multi

#define SUBSCRIBERS_PER_MACHINE 1000000	/* 1 million subscribers per machine */

/* SHM keys for tables: (f + 1) keys per table at every machine */
#define SUBSCRIBER_BASE_SHM_KEY 2000
#define SEC_SUBSCRIBER_BASE_SHM_KEY 4000
#define ACCESS_INFO_BASE_SHM_KEY 6000
#define SPECIAL_FACILTY_BASE_SHM_KEY 8000
#define CALL_FORWARDING_BASE_SHM_KEY 10000
#define TATP_MAX_SHM_KEY 12000

// TATP table keys and values
// All keys have been sized to 8 bytes
// All values have been sized to the next multiple of 8 bytes

/*
 * SUBSCRIBER table
 * Primary key: <uint32_t s_id>
 * Value size: 40 bytes. Full value read in GET_SUBSCRIBER_DATA.
 */
union tatp_sub_key_t {
	struct {
		uint32_t s_id;
		uint8_t unused[4];
	};
	hots_key_t hots_key;

	tatp_sub_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(tatp_sub_key_t) == sizeof(hots_key_t), "");

struct tatp_sub_val_t {
	tatp_sub_nbr_t sub_nbr;
	char sub_nbr_unused[7];	/* sub_nbr should be 15 bytes. We used 8 above. */
	char hex[5];
	char bytes[10];
	short bits;
	uint32_t msc_location;
	uint32_t vlr_location;
};
static_assert(sizeof(tatp_sub_val_t) == 40, "");


/*
 * Secondary SUBSCRIBER table
 * Key: <tatp_sub_nbr_t>
 * Value size: 8 bytes
 */
union tatp_sec_sub_key_t {
	tatp_sub_nbr_t sub_nbr;
	hots_key_t hots_key;

	tatp_sec_sub_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(tatp_sec_sub_key_t) == sizeof(hots_key_t), "");

struct tatp_sec_sub_val_t {
	uint32_t s_id;
	uint8_t magic;
	uint8_t unused[3];
};
static_assert(sizeof(tatp_sec_sub_val_t) == 8, "");


/*
 * ACCESS INFO table
 * Primary key: <uint32_t s_id, uint8_t ai_type>
 * Value size: 16 bytes
 */
union tatp_accinf_key_t {
	struct {
		uint32_t s_id;
		uint8_t ai_type;
		uint8_t unused[3];
	};
	hots_key_t hots_key;

	tatp_accinf_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(tatp_accinf_key_t) == sizeof(hots_key_t), "");

struct tatp_accinf_val_t {
	char data1;
	char data2;
	char data3[3];
	char data4[5];
	uint8_t unused[6];
}; 
static_assert(sizeof(tatp_accinf_val_t) == 16, "");


/*
 * SPECIAL FACILITY table
 * Primary key: <uint32_t s_id, uint8_t sf_type>
 * Value size: 8 bytes
 */
union tatp_specfac_key_t {
	struct {
		uint32_t s_id;
		uint8_t sf_type;
		uint8_t unused[3];
	};
	hots_key_t hots_key;

	tatp_specfac_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(tatp_specfac_key_t) == sizeof(hots_key_t), "");

struct tatp_specfac_val_t {
	char is_active;
	char error_cntl;
	char data_a;
	char data_b[5];
};
static_assert(sizeof(tatp_specfac_val_t) == 8, "");


/*
 * CALL FORWARDING table
 * Primary key: <uint32_t s_id, uint8_t sf_type, uint8_t start_time>
 * Value size: 16 bytes
 */
union tatp_callfwd_key_t {
	struct {
		uint32_t s_id;
		uint8_t sf_type;
		uint8_t start_time;
		uint8_t unused[2];
	};
	hots_key_t hots_key;

	tatp_callfwd_key_t()
	{
		hots_key = 0;
	}
};
static_assert(sizeof(tatp_callfwd_key_t) == sizeof(hots_key_t), "");

struct tatp_callfwd_val_t {
	uint8_t end_time;
	char numberx[15];
};
static_assert(sizeof(tatp_callfwd_val_t) == 16, "");


// Magic numbers for debugging. These are unused in the spec.
#define TATP_MAGIC 97	/* Some magic number <= 255 */
#define tatp_sub_msc_location_magic (TATP_MAGIC)
#define tatp_sec_sub_magic (TATP_MAGIC + 1)
#define tatp_accinf_data1_magic (TATP_MAGIC + 2)
#define tatp_specfac_data_b0_magic (TATP_MAGIC + 3)
#define tatp_callfwd_numberx0_magic (TATP_MAGIC + 4)


// Helpers for generating workload
#define TATP_TXN_TYPES 7
enum class tatp_txn_type_t : int {
	get_subsciber_data,
	get_access_data,
	get_new_destination,
	update_subscriber_data,
	update_location,
	insert_call_forwarding,
	delete_call_forwarding,
};

#endif /* TATP_DEFS */
