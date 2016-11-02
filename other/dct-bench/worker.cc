#include "libhrd/hrd.h"
#include "util/rte_memcpy.h"
#include "main.h"

__thread int wrkr_gid;	/* Global ID of this worker */
__thread int wrkr_lid;	/* Local ID of this worker */
__thread int wrkr_port;	/* 0-based port used by this worker */
__thread int num_ports, num_qps, window_size, numa_node;
__thread int num_machines, workers_per_machine, num_workers;

__thread size_t req_total = 0;
__thread uint64_t seed = 0xdeadbeef;

/* Application parameters */
__thread global_stats_t *global_stats;
__thread size_t size_req;

__thread struct timespec msr_start, msr_end;	/* Throughput measurement */
__thread double lat_sum_us;	/* For latency measurement */

/* Each thread records the DCT target at each machine */
__thread dct_attr_t *remote_dct_attr[HOTS_MAX_MACHINES];
__thread struct ibv_ah *ah[HOTS_MAX_MACHINES];

/* RDMA stuff */
__thread struct ibv_qp *qp[DCTBENCH_MAX_QPS];
__thread struct ibv_cq *cq[DCTBENCH_MAX_QPS];
__thread struct ibv_pd *pd;
__thread struct ibv_mr *mr;
__thread uint8_t *rdma_buf;
__thread int qp_for_window[DCTBENCH_MAX_WINDOW];	/* For polling */

void slave_func()
{
	long long iters = 0;
	struct timespec lat_start[DCTBENCH_MAX_WINDOW];	/* For latency */
	_unused(lat_start);	/* Unused if latency measurement is disabled */

	int qp_i = 0;	/* The QP to use */

	clock_gettime(CLOCK_REALTIME, &msr_start);
	printf("Worker %d: Executing slave\n", wrkr_gid);

	while(1) {
		int window_i = iters % window_size;
		qp_for_window[window_i] = qp_i;

#if DCTBENCH_MEASURE_LATENCY == 1
		clock_gettime(CLOCK_REALTIME, &lat_start[window_i]);
#endif

		struct ibv_exp_send_wr wr, *bad_wr;
		struct ibv_wc wc;
		struct ibv_sge sg_list;

#if DCTBENCH_SINGLE_THREADED_OUTBOUND_PERF == 1
		int mn = ((mn + 1) % (num_machines - 1)) + 1;
		int mn = qp_i + 1;
#else
		int mn = hrd_fastrand(&seed) % num_machines;
#endif

		assert(mn < num_machines);
		int offset = hrd_fastrand(&seed) % (DCTBENCH_BUF_SIZE - size_req * 2);

		wr.next = NULL;
		wr.num_sge = 1;
		wr.exp_opcode = IBV_EXP_WR_RDMA_READ;
		wr.exp_send_flags = IBV_EXP_SEND_SIGNALED;

		/* Local buf */
		sg_list.addr = (uint64_t) (unsigned long) rdma_buf + offset;
		sg_list.length = size_req;
		sg_list.lkey = mr->lkey;

		/* Remote */
		wr.wr.rdma.remote_addr = remote_dct_attr[mn]->buf_addr + offset;
		wr.wr.rdma.rkey = remote_dct_attr[mn]->rkey;

		/* Routing */
		wr.sg_list = &sg_list;
		wr.dc.ah = ah[mn];
		wr.dc.dct_access_key = DCTBENCH_DCT_KEY;
		wr.dc.dct_number = remote_dct_attr[mn]->dct_num;

		int err = ibv_exp_post_send(qp[qp_i], &wr, &bad_wr);
		if(err != 0) {
			fprintf(stderr, "Failed to post send. Error = %d\n", err);
			assert(false);
		}

		/* Must increment before polling and measuring latency */
		HRD_MOD_ADD(qp_i, num_qps);
		iters++;
		assert(qp_i == iters % num_qps);	/* window_size % num_qps == 0 */

		/* Poll a completion */
		if(iters >= (unsigned) window_size) {
			int num_comps = 0;
			while(1) {
				int prev_qp = qp_for_window[iters % window_size];

				num_comps += ibv_poll_cq(cq[prev_qp], 1, &wc);
				if(num_comps == 1) {
					assert(wc.status == IBV_WC_SUCCESS);
					break;
				}
			}

#if DCTBENCH_MEASURE_LATENCY == 1
			int comp_window_i = iters & (window_size - 1);
			struct timespec lat_end;
			clock_gettime(CLOCK_REALTIME, &lat_end);
			auto &_lat_start = lat_start[comp_window_i];
			double _lat =
				(lat_end.tv_sec - _lat_start.tv_sec) * 1000000 +
				(double) (lat_end.tv_nsec - _lat_start.tv_nsec) / 1000;

			lat_sum_us += _lat;
#endif
		}

		if((iters & M_1_) == M_1_) {
			clock_gettime(CLOCK_REALTIME, &msr_end);

			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;

			/* Fill in this worker's global stats */
			auto &gs = global_stats[wrkr_lid];
			gs.req_rate = (double) M_1 / msr_usec;

			printf("Worker %d: requests/s = %.2f M/s. Average lat = %.2f us\n",
				wrkr_lid, gs.req_rate, lat_sum_us / M_1);
			lat_sum_us = 0;

			if(wrkr_lid == 0) {
				double req_rate_tot = 0;

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					req_rate_tot += global_stats[wrkr_i].req_rate;
				}

				hrd_red_printf("Machine req rate = %.2f M/s\n", req_rate_tot);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);
		}
	}
}

// Parse configuration. Requires wrkr_gid to be set.
void parse_config()
{
	auto test_config =
		::mica::util::Config::load_file("dct-test.json").get("dct_test");

	num_ports = (int) test_config.get("num_ports").get_int64();
	num_qps = (int) test_config.get("num_qps").get_int64();
	window_size = (int) test_config.get("window_size").get_int64();
	numa_node = (int) test_config.get("numa_node").get_int64();
	num_machines = test_config.get("num_machines").get_int64();
	workers_per_machine = test_config.get("workers_per_machine").get_int64();
	size_req = (int) test_config.get("size_req").get_int64();
	
	assert(num_ports >= 1 && num_ports <= 2);
	assert(num_qps >= 1 && num_qps <= DCTBENCH_MAX_QPS);
	assert(window_size >= 1 && window_size <= DCTBENCH_MAX_WINDOW);

#if DCTBENCH_SINGLE_THREADED_OUTBOUND_PERF == 1
	/*
	 * In single-threaded performance measurement mode, we "connect" @qp_i to
	 * machine (@qp_i + 1).
	 */
	//assert(num_qps < num_machines);
#endif

	assert(numa_node >= 0 && numa_node <= 3);
	assert(num_machines >= 0 && num_machines <= 256);
	assert(workers_per_machine >= 1 && workers_per_machine <= 56);
	assert(size_req > 0 && size_req <= 256 && size_req % sizeof(uint64_t) == 0);
	assert(size_req <= DCTBENCH_MAX_INLINE);

	// Derived parameters
	num_workers = num_machines * workers_per_machine;
	wrkr_lid = wrkr_gid % workers_per_machine;
	wrkr_port = wrkr_lid % num_ports;	/* A worker uses one port */
}

void create_dct_initiators()
{
	struct ibv_device **dev_list = ibv_get_device_list(NULL);
	assert(dev_list != NULL);

	struct ibv_device *ib_dev = dev_list[0];	/* Use the 1st device */
	struct ibv_context *ctx = ibv_open_device(ib_dev);
	assert(ctx != NULL);

	/* Create and register an RDMA buffer */
	pd = ibv_alloc_pd(ctx);
	assert(pd != NULL);

	rdma_buf = (uint8_t *) malloc(DCTBENCH_BUF_SIZE);
	assert(rdma_buf != NULL);

	mr = ibv_reg_mr(pd, rdma_buf, DCTBENCH_BUF_SIZE,
		IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_READ);
	assert(mr != NULL);

	/* Create CQs and DC initiators */
	for(int i = 0; i < num_qps; i++) {
		cq[i] = ibv_create_cq(ctx, 128, NULL, NULL, 0);
		assert(cq[i] != NULL);

		/* Create the DCT initiator */
		struct ibv_qp_init_attr_ex create_attr;
		memset((void *) &create_attr, 0, sizeof(create_attr));
		create_attr.send_cq = cq[i];
		create_attr.recv_cq = cq[i];
		create_attr.cap.max_send_wr = 2 * window_size;
		create_attr.cap.max_send_sge = 1;
		create_attr.cap.max_inline_data = size_req;
		create_attr.qp_type = IBV_EXP_QPT_DC_INI;
		create_attr.pd = pd;
		create_attr.comp_mask = IBV_QP_INIT_ATTR_PD;

		qp[i] = ibv_create_qp_ex(ctx, &create_attr);
		assert(qp[i] != NULL);
		
		/* Modify QP to init */
		struct ibv_exp_qp_attr modify_attr;
		memset((void *) &modify_attr, 0, sizeof(modify_attr));
		modify_attr.qp_state = IBV_QPS_INIT;
		modify_attr.pkey_index = 0;
		modify_attr.port_num = wrkr_port + 1;
		modify_attr.qp_access_flags = 0;
		modify_attr.dct_key = DCTBENCH_DCT_KEY;

		if (ibv_exp_modify_qp(qp[i], &modify_attr,
			IBV_EXP_QP_STATE | IBV_EXP_QP_PKEY_INDEX |
			IBV_EXP_QP_PORT | IBV_EXP_QP_DC_KEY)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			assert(false);
		}

		/* Modify QP to RTR */
		modify_attr.qp_state = IBV_QPS_RTR;
		modify_attr.max_dest_rd_atomic = 0;
		modify_attr.path_mtu = IBV_MTU_4096;
		modify_attr.ah_attr.is_global = 0;

		/* Initially, connect to the DCT target on machine 0 */
		modify_attr.ah_attr.dlid = remote_dct_attr[0]->lid;
		modify_attr.ah_attr.port_num = wrkr_port + 1;	/* Local port */
		modify_attr.ah_attr.sl = 0;
		modify_attr.dct_key = DCTBENCH_DCT_KEY;

		if (ibv_exp_modify_qp(qp[i], &modify_attr, IBV_EXP_QP_STATE |
			//IBV_EXP_QP_MAX_DEST_RD_ATOMIC | IBV_EXP_QP_PATH_MTU |
			IBV_EXP_QP_PATH_MTU |
			IBV_EXP_QP_AV)) {
			fprintf(stderr, "Failed to modify QP to RTR\n");
			assert(false);
		}

		/* Modify QP to RTS */
		modify_attr.qp_state = IBV_QPS_RTS;
		modify_attr.timeout = 14;
		modify_attr.retry_cnt = 7;
		modify_attr.rnr_retry = 7;
		modify_attr.max_rd_atomic = 16;
		if (ibv_exp_modify_qp(qp[i], &modify_attr,
			IBV_EXP_QP_STATE | IBV_EXP_QP_TIMEOUT | IBV_EXP_QP_RETRY_CNT |
			IBV_EXP_QP_RNR_RETRY | IBV_EXP_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify QP to RTS\n");
			assert(false);
		}
	}


	// Create address handles. This requires the protection domain created above
	for(int mc_i = 0; mc_i < num_machines; mc_i++) {
		/* Use the remote LID to create an address handle for it */
		struct ibv_ah_attr ah_attr;
		memset((void *) &ah_attr, 0, sizeof(ah_attr));
		ah_attr.is_global = 0;
		ah_attr.dlid = remote_dct_attr[mc_i]->lid;
		ah_attr.sl = 0;
		ah_attr.src_path_bits = 0;
		ah_attr.port_num = wrkr_port + 1;
		ah[mc_i] = ibv_create_ah(pd, &ah_attr);
		assert(ah[mc_i] != NULL);
	}
}

void resolve_servers()
{
	for(int mc_i = 0; mc_i < num_machines; mc_i++) {
		char dct_target_name[HRD_QP_NAME_SIZE];
		sprintf(dct_target_name, "target-%d-%d", mc_i, wrkr_port);

		/* Get the DCT info for this machine */
		while(1) {
			int val_len = hrd_get_published(dct_target_name,
				(void **) &remote_dct_attr[mc_i]);
			if(val_len > 0 ) {
				assert(val_len == sizeof(dct_attr_t));

			printf("Worker %d resolved machine %d. Remote info: "
				"lid = %u, dct_num = %u, buf_addr = %p, rkey = %u\n",
				wrkr_gid, mc_i,
				(unsigned) remote_dct_attr[mc_i]->lid,
				(unsigned) remote_dct_attr[mc_i]->dct_num,
				(void *) remote_dct_attr[mc_i]->buf_addr,
				remote_dct_attr[mc_i]->rkey);

				break;
			} else {
				sleep(.1);
			}	
		}

	}
}

void run_thread(struct thread_params *params)
{
	/* Intialize TLS variables */
	wrkr_gid = params->wrkr_gid;
	global_stats = params->global_stats;
	parse_config();

	/* Move fastrand for this thread */
	for(int i = 0; i < wrkr_gid * 1000000; i++) {
		hrd_fastrand(&seed);
	}

	resolve_servers();
	create_dct_initiators();

	slave_func();
}
