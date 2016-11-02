#ifndef MAIN_H
#define MAIN_H

#include "lockserver/lockserver.h"
#include "logger/logger.h"
#include "datastore/fixedtable/ds_fixedtable.h"

/* SHM keys for tables: (f + 1) * 2 keys per table per thread */
#define DIST_KV_BASE_SHM_KEY 2000
#define DIST_KV_MAX_SHM_KEY 4000

// Debug macros
#define TXTEST_DPRINTF 0
#define txtest_dprintf(fmt, ...) \
	do { \
		if (TXTEST_DPRINTF) { \
			fprintf(stderr, fmt, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)

struct global_stats_t {
	double tx_tput;
	double req_rate;
	double creq_rate;
	double max_lat_thread; /* Max latency (us) for all coros in this thread */
	double pad[4];

	global_stats_t()
	{
		tx_tput = 0;
		req_rate = 0;
		creq_rate = 0;
		max_lat_thread = 0;
	}
};

static_assert(sizeof(global_stats_t) == 64, "");

struct thread_params {
	int wrkr_gid;
	FixedTable **fixedtable;
	Lockserver *lockserver;

	global_stats_t *global_stats;
};

void run_thread(struct thread_params *params);

/* Bitwise operations for 64-bit bitmask */
#define bit_set(x, y) (x |= (1ull << y))
#define is_set(x, y) (x & (1ull << y))

#endif
