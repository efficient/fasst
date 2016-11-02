#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdlib.h>

#include "stress.h"
#include "logger/logger.h"
#include "datastore/fixedtable/ds_fixedtable.h"

struct global_stats_t {
	double tx_tput;
	double req_rate;
	double creq_rate;
	double pad[5];

	global_stats_t()
	{
		tx_tput = 0;
		req_rate = 0;
		creq_rate = 0;
	}
};

static_assert(sizeof(global_stats_t) == 64, "");

struct thread_params {
	int wrkr_gid;
	Stress *stress;

	global_stats_t *global_stats;
};

void run_thread(struct thread_params *params);

#endif
