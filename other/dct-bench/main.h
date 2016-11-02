#ifndef __DC_H
#define __DC_H

#include <getopt.h>
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hots.h"
#include "libhrd/hrd.h"

/* Json */
#include "mica/util/config.h"

#define DCTBENCH_MEASURE_LATENCY 1

#define DCTBENCH_MAX_QPS 32 // Max DC initiators per worker
#define DCTBENCH_DCT_KEY 3185
#define DCTBENCH_BUF_SIZE 4096	// Size of the registered buffer
#define DCTBENCH_MAX_WINDOW 128
#define DCTBENCH_MAX_INLINE 60

/*
 * Measure the outbound RDMA throughput of a single thread on machine 0.
 * This is useful to understand the raw overhead of using DCT.
 *
 * In this mode, only machine 0 issues operations. All opearations on @qp_i
 * are issued to machin (@qp_i + 1) to prevent dynamic connect/disconnect
 * messages.
 */
#define DCTBENCH_SINGLE_THREADED_OUTBOUND_PERF 0

struct global_stats_t {
	double req_rate;
	double pad[7];

	global_stats_t()
	{
		req_rate = 0;
	}
};
static_assert(sizeof(global_stats_t) == 64, "");


struct thread_params {
	int wrkr_gid;
	global_stats_t *global_stats;
};

void run_thread(struct thread_params *params);

/* Info published by the DCT target */
struct dct_attr_t {
	int lid;
	int dct_num;

	/* Info about the RDMA buffer associated with this QP */
	uintptr_t buf_addr;
	uint32_t rkey;
};

static inline void check_dct_supported(struct ibv_context *ctx)
{
	printf("Checking if DCT is supported.. ");
	struct ibv_exp_device_attr dattr;

	dattr.comp_mask = IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS |
					  IBV_EXP_DEVICE_DC_RD_REQ | IBV_EXP_DEVICE_DC_RD_RES;
	int err = ibv_exp_query_device(ctx, &dattr);
	if (err) {
		printf("couldn't query device extended attributes\n");
		assert(false);
	} else {
		if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS)) {
			printf("no extended capability flgas\n");
			assert(false);
		}
		if (!(dattr.exp_device_cap_flags & IBV_EXP_DEVICE_DC_TRANSPORT)) {
			printf("DC transport not enabled\n");
			assert(false);
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_REQ)) {
			printf("no report on max requestor rdma/atomic resources\n");
			assert(false);
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_RES)) {
			printf("no report on max responder rdma/atomic resources\n");
			assert(false);
		}
	}

	printf("Success\n");
}

static inline void check_dct_healthy(struct ibv_exp_dct *dct)
{
	struct ibv_exp_dct_attr dcqattr;
	dcqattr.comp_mask = 0;

	int err = ibv_exp_query_dct(dct, &dcqattr);
	if (err) {
		printf("query dct failed\n");
		assert(false);
	} else if (dcqattr.dc_key != DCTBENCH_DCT_KEY) {
		printf("queried dckry (0x%llx) is different then provided at "
			   "create (0x%llx)\n",
			   (unsigned long long) dcqattr.dc_key,
			   (unsigned long long) DCTBENCH_DCT_KEY);
		assert(false);
	} else if (dcqattr.state != IBV_EXP_DCT_STATE_ACTIVE) {
		printf("state is not active %d\n", dcqattr.state);
		assert(false);
	}
}

#endif /* __DC_H */
