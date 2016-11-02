#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdlib.h>

#define CHECK_RESP 0
#define USE_UNIQUE_WORKERS 1 /* All requests in batch are to different workers */

struct global_stats_t {
	double req_rate;
	double creq_rate;
	double pad[6];

	global_stats_t()
	{
		req_rate = 0;
		creq_rate = 0;
	}
};
static_assert(sizeof(global_stats_t) == 64, "");

#define MAX_REQ_BATCH_SIZE 16

struct thread_params {
	int wrkr_gid;
	global_stats_t *global_stats;
};

void run_thread(struct thread_params *params);

#endif
