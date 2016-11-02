#include "libhrd/hrd.h"
#include "rpc/rpc.h"

Rpc::Rpc(struct rpc_args args) : info(args)
{
	hrd_red_printf("Rpc: Initializing for worker %d, required recvs = %d\n",
		info.wrkr_gid, required_recvs());
	fflush(stdout);

	/* Sanity checks */
	check_defines();
	check_info();

	pr_port = info.base_port_index + (info.wrkr_lid % info.num_ports);
	assert(pr_port < HOTS_MAX_PORTS);

	int shm_key = RPC_BASE_SHM_KEY + info.wrkr_lid;
	assert(shm_key < RPC_MAX_SHM_KEY);

	cb = hrd_ctrl_blk_init(info.wrkr_gid, /* local hid */
		pr_port, 0, /* port index, numa node */
		0, 0, /* conn qps, UC */
		NULL, 0, -1, /* conn prealloc buf, buf size, conn buf shm key */
		NULL, info.num_qps,	/* dgram prealloc buf, dgram qps */
		RPC_RECV_BUF_SIZE + RPC_MBUF_SPACE + RPC_NON_INLINE_SPACE, /* buf size */
		shm_key);	/* dgram buf shm key */

	/* Save lkey to avoid indexing into dgram_buf_mr repeatedly */
	lkey = cb->dgram_buf_mr->lkey;

	init_non_zero_members();
	init_coroutine_metadata();

	/* Initialize RECV freelist and constant fields of wr's */
	init_recv_wrs();
	init_send_wrs();

	check_modded();

	/* Publish the QP */
	post_recvs(HRD_RQ_DEPTH);	/* Fill RECVs before publishing */

	/* Publish 0th QP for remote workers to post SENDs to */
	char my_qp_name[HRD_QP_NAME_SIZE];
	sprintf(my_qp_name, "rpc-worker-%d-%d", info.machine_id, info.wrkr_lid);
	hrd_publish_dgram_qp(cb, 0, my_qp_name); /* Only QP 0 is exposed for RECVs */
	rpc_dprintf("Rpc: Worker %s published.\n", my_qp_name);

	/* Resolve remote machines and save their QPNs */
	for(int mc_i = 0; mc_i < info.num_machines; mc_i++) {
		char rem_qp_name[HRD_QP_NAME_SIZE];	/* Peer QP on machine @mc_i */
		sprintf(rem_qp_name, "rpc-worker-%d-%d", mc_i, info.wrkr_lid);

		struct hrd_qp_attr *rem_qp = NULL;
		while(rem_qp == NULL) {
			rem_qp = hrd_get_published_qp(rem_qp_name);
			if(rem_qp == NULL) {
				usleep(20000);
			}
		}

		/* We only need to save the remote QPN. LID is saved in ah. */
		rem_qpn[mc_i] = rem_qp->qpn;

		rpc_dprintf("Rpc: Worker %d resolved %s (%d of %d machines).\n",
			info.wrkr_gid, rem_qp_name, mc_i, info.num_machines);
		
		struct ibv_ah_attr ah_attr;
		memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
		ah_attr.is_global = 0;
		ah_attr.dlid = rem_qp->lid;
		ah_attr.sl = 0;
		ah_attr.src_path_bits = 0;
		ah_attr.port_num = cb->dev_port_id;	/* Local port */

		ah[mc_i] = ibv_create_ah(cb->pd, &ah_attr);
		assert(ah[mc_i] != NULL);
	}

	hrd_red_printf("Rpc: initialization done. Closing memcached connection.!\n");
	fflush(stdout);

	hrd_close_memcached();	/* Close memcached connections for this thread */
}

/* Register a handler and an optional argument for a request type. */
void Rpc::register_rpc_handler(int req_type,
	size_t (*func)(uint8_t *resp_buf, rpc_resptype_t *resp_type,
		const uint8_t *req_buf, size_t req_len, void *), void *arg)
{
	assert(func != NULL);
	if(!RPC_IS_VALID_TYPE(req_type)) {
		printf("Rpc: Error. Cannot register handler for non-request types.\n");
		exit(-1);
	}

	if(rpc_handler[req_type] != NULL || rpc_handler_arg[req_type] != NULL) {
		printf("Rpc: Error. Function for handler type %d already registered.\n",
			req_type);
		exit(-1);
	}

	if(arg == NULL) {
		printf("Rpc: Warning. Handler for request type %d assigned NULL arg.\n",
			req_type);
	}

	rpc_handler[req_type] = func;	/* Actually register */
	rpc_handler_arg[req_type] = arg;
}

void Rpc::print_stats()
{
#if RPC_COLLECT_STATS == 0
	printf("Rpc: Stats disabled, ");
#else
	printf("Rpc: Worker %d: Average response batch size = %.2f, "
		"wasted poll cq = %lu, ", info.wrkr_gid,
		(float) stat_num_cresps / stat_resp_post_send_calls,
		stat_wasted_poll_cq);
#endif

	/* Print timer info in the same line as stats */
#if RPC_COLLECT_TIMER_INFO == 0
	printf("timer info not available\n");
#else
	printf("cycles per RECV = %.2f\n",
		(float) tot_cycles_post_recv / stat_num_recvs);
#endif

	/* Do not zero-out request stats that are used directly by apps */
	stat_num_cresps = 0;
	stat_resp_post_send_calls = 0;
	stat_wasted_poll_cq = 0;
	stat_num_recvs = 0;
	tot_cycles_post_recv = 0;
}

/* Compile time checks */
void Rpc::check_defines()
{
#if RPC_COLLECT_TIMER_INFO == 1
	/* Timer info relies on rpc_stat_inc */
	ct_assert(RPC_COLLECT_STATS == 1);
#endif
	ct_assert(HRD_SQ_DEPTH >= 2 * RPC_UNSIG_BATCH);	/* Queue capacity check */
	ct_assert(HRD_RQ_DEPTH >= 2 * RPC_UNSIG_BATCH);	/* Non-inlined buffers */
	
	/* Checks for immediate bit encoding */
	ct_assert(bit_capacity(RPC_NUM_REQS_BITS) >= RPC_MAX_MSG_CORO);
	ct_assert(bit_capacity(HOTS_CORO_ID_BITS) >= RPC_MAX_CORO);
}

/* Check constructor args */
void Rpc::check_info()
{
	range_assert(info.wrkr_gid, 0, info.num_workers - 1);
	range_assert(info.wrkr_lid, 0, HOTS_MAX_SERVER_THREADS);
	range_assert(info.num_workers, 2, HOTS_MAX_WORKERS);
	range_assert(info.num_coro, 2, RPC_MAX_CORO);	/* At least 1 slave coro */
	range_assert(info.base_port_index, 0, 8);
	range_assert(info.num_ports, 1, HOTS_MAX_PORTS);
	range_assert(info.num_qps, 1, RPC_MAX_QPS);
	range_assert(info.numa_node, 0, 1);
	range_assert(info.postlist, 1, RPC_MAX_POSTLIST);
	range_assert(info.max_pkt_size, 1, RPC_MAX_MAX_PKT_SIZE);

	assert(required_recvs() <= HRD_RQ_DEPTH);
}

/* Most class members are zero-ed in the declaration. Init the rest here. */
void Rpc::init_non_zero_members()
{
	pr_port = info.wrkr_gid % info.num_ports;

	/* RECV targets lie in separate cachelines for low L3--PCIe traffic */
	recv_step = 0;
	while(recv_step < info.max_pkt_size + HOTS_GRH_BYTES) {
		 recv_step += 64;
	}
	assert(recv_step <= RPC_MAX_MAX_PKT_SIZE);

	/* RECV slack */
	recv_slack = HRD_RQ_DEPTH - required_recvs() - 1;
	if(recv_slack >= RPC_MIN_RECV_SLACK) {
		recv_slack = RPC_MIN_RECV_SLACK;
	}
	assert(recv_slack >= RPC_MIN_RECV_SLACK);

	// Loss detection
	clock_gettime(CLOCK_REALTIME, &rpc_init_time);
	clock_gettime(CLOCK_REALTIME, &ld_stopwatch);

	char ld_filename[100];
	sprintf(ld_filename, "/tmp/fasst-rpc-pkt-loss-wrkr-%d", info.wrkr_gid);
	ld_fp = fopen(ld_filename, "w");
	assert(ld_fp != NULL);
}

/* This must be called after initializing libhrd control blocks */
void Rpc::init_coroutine_metadata()
{
	next_coro[RPC_MASTER_CORO_ID] = RPC_MASTER_CORO_ID;
	resp_batch.num_cresps = 0;

	// We registered memory for RPC mbufs with the control blocks. Use it here.
	uint8_t *_base = (uint8_t *) &cb->dgram_buf[RPC_RECV_BUF_SIZE];
	size_t _off = 0;
	size_t _step = info.max_pkt_size;
	_step = (_step + 63) & ~63ull;	/* Round to next mul of 64 */
	assert(_step >= info.max_pkt_size && _step % 64 == 0);

	// Initialize resp_batch (for master coroutine) with @HRD_RQ_DEPTH messages
	for(int msg_i = 0; msg_i < HRD_RQ_DEPTH; msg_i++) {
		rpc_cmsg_t *cmsg = &resp_batch.cmsg_arr[msg_i];

		cmsg->remote_mn = RPC_INVALID_MN;
		cmsg->num_centry = 0;
		
		/* Master does not need request mbufs. Insert invalid values. */
		cmsg->req_mbuf.alloc_buf = NULL;
		cmsg->req_mbuf.alloc_len = 0;
		cmsg->req_mbuf.cur_buf = NULL;

		assert(_off < RPC_MBUF_SPACE - _step);	/* Check space */
		cmsg->resp_mbuf.alloc_with_buf((uint8_t *) &_base[_off],
			info.max_pkt_size);
		_off += _step;
	}

	// Initialize req_batch structs (for slave coroutines)
	for(int coro_i = 1; coro_i < info.num_coro; coro_i++) {
		assert(coro_i != RPC_MASTER_CORO_ID);

		rpc_req_batch_t *req_batch = &req_batch_arr[coro_i];

		/* Manually reset the req batch - can't use @req_batch->clear() here */
		req_batch->num_reqs = 0;
		req_batch->num_reqs_done = 0;
		req_batch->num_uniq_mn = 0;
		for(int mc_i = 0; mc_i < HOTS_MAX_MACHINES; mc_i++) {
			req_batch->cmsg_for_mc[mc_i] = -1;
		}

		/* Slaves need both request and response coalesced mbufs */
		for(int msg_i = 0; msg_i < RPC_MAX_MSG_CORO; msg_i++) {
			rpc_cmsg_t *cmsg = &req_batch->cmsg_arr[msg_i];
			/* remote_wn must be invalidated for correct coalescing */
			cmsg->remote_mn = RPC_INVALID_MN;
			cmsg->num_centry = 0;

			/* Allocate request mbuf */
			assert(_off < RPC_MBUF_SPACE - _step);	/* Check space */
			cmsg->req_mbuf.alloc_with_buf((uint8_t *) &_base[_off],
				info.max_pkt_size);
			_off += _step;
			
			/* Allocate response mbuf - response is copied into this */
			assert(_off < RPC_MBUF_SPACE - _step);	/* Check space */
			cmsg->resp_mbuf.alloc_with_buf((uint8_t *) &_base[_off],
				info.max_pkt_size);
			_off += _step;
		}
	}

	// Initialize non-inline buffers for resp_batch

	/* Reset _base and _off to a different memory region. _step remains same */
	_base = (uint8_t *) &cb->dgram_buf[RPC_RECV_BUF_SIZE + RPC_MBUF_SPACE];
	_off = 0;
	resp_batch.non_inline_index = 0;
	for(int index = 0; index < HRD_RQ_DEPTH; index++) {
		assert(_off < RPC_NON_INLINE_SPACE - _step);
		resp_batch.non_inline_bufs[index] = (uint8_t *) &_base[_off];
		_off += _step;
	}
}

/* Initialize constant fields of send wr's */
void Rpc::init_send_wrs()
{
	for(int wr_i = 0; wr_i < RPC_MAX_POSTLIST; wr_i++) {
		send_sgl[wr_i].lkey = lkey;

		send_wr[wr_i].next = &send_wr[wr_i + 1];
		send_wr[wr_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;
		send_wr[wr_i].opcode = IBV_WR_SEND_WITH_IMM;
		send_wr[wr_i].num_sge = 1;
		send_wr[wr_i].sg_list = &send_sgl[wr_i];
	}
}

/* Initialize constant fields of recv wr's into a circular list */
void Rpc::init_recv_wrs()
{
	assert(recv_step > 0 && recv_step % 64 == 0);
	assert(recv_step * info.num_ports * HRD_RQ_DEPTH <= RPC_RECV_BUF_SIZE);

	assert(cb != NULL);

	for(int wr_i = 0; wr_i < HRD_RQ_DEPTH; wr_i++) {
		int offset = (wr_i * recv_step) + (64 - HOTS_GRH_BYTES);
		assert(offset + info.max_pkt_size + 64 < RPC_RECV_BUF_SIZE);

		recv_sgl[wr_i].length = recv_step;
		recv_sgl[wr_i].lkey = cb->dgram_buf_mr->lkey;
		recv_sgl[wr_i].addr = (uintptr_t) &cb->dgram_buf[offset];

		recv_wr[wr_i].wr_id = recv_sgl[wr_i].addr;/* Debug */
		recv_wr[wr_i].sg_list = &recv_sgl[wr_i];
		recv_wr[wr_i].num_sge = 1;

		/* Circular link */
		recv_wr[wr_i].next = (wr_i < HRD_RQ_DEPTH - 1) ?
			&recv_wr[wr_i + 1] : &recv_wr[0];
	}
}

/* Check if it is safe to use modded driver if it is requested. */
void Rpc::check_modded() {
	if(RPC_ENABLE_MODDED_DRIVER == 1 && info.wrkr_lid == 0) {
		printf("Verifying that the driver is modded..\n");
		struct ibv_recv_wr special_wr;
		special_wr.wr_id = HOTS_MODDED_PROBE_WRID;

		struct ibv_recv_wr *bad_wr = &special_wr;
		int probe_ret = ibv_post_recv(cb->dgram_qp[0], NULL, &bad_wr);
		if(probe_ret != HOTS_MODDED_PROBE_RET) {
			hrd_red_printf("\nHoTS: Error: Driver not modded. "
				"Probe returned %d.\n", probe_ret);
			exit(-1);
		}
	}
}

/*
 * We should have enough RECVs for the following:
 * 1. (info.num_coro - 1) * RPC_MAX_MSG_CORO responses to our requests.
 * 2. o(num_machines * (info.num_coro - 1)) reqs from remote threads.
 */
int Rpc::required_recvs()
{
	int num_machines = info.num_workers / info.workers_per_machine;
	return (info.num_coro - 1) * RPC_MAX_MSG_CORO +
		num_machines * (info.num_coro - 1);
}

/* Basic checks for a RECVd immediate. Debug-only. */
void Rpc::check_imm(union rpc_imm imm)
{
	/* @is_req field is automatically valid */
	rpc_dassert(imm.num_reqs >= 1 && imm.num_reqs <= RPC_MAX_MSG_CORO);
	rpc_dassert(imm.mchn_id >= 0 && imm.mchn_id < info.num_machines);
	rpc_dassert(imm.coro_id >= 0 && imm.coro_id < info.num_coro);
}

/* Check if a coalesced buffer is valid by examining the headers */
void Rpc::check_coalesced_msg(uint8_t *cbuf, int num_centry)
{
	rpc_dassert(num_centry > 0);

	/* Check if the requests in the coalesced message are well-formed */
	size_t offset = 0;	/* Offset into the request mbuf */
	for(int i = 0; i < num_centry; i++) {
		rpc_cmsg_reqhdr_t *cmsg_reqhdr =
			(rpc_cmsg_reqhdr_t *) &cbuf[offset];

		rpc_dassert(RPC_IS_VALID_TYPE(cmsg_reqhdr->req_type));
		rpc_dassert(cmsg_reqhdr->coro_seqnum <= RPC_MAX_MSG_CORO);
		rpc_dassert(cmsg_reqhdr->magic == RPC_CMSG_REQ_HDR_MAGIC);
		rpc_dassert(cmsg_reqhdr->size >= 0 &&
			/*
			 * Individual (non-coalesced) requests/responses need to be
			 * strictly smaller than max_pkt_size because of the prefixed
			 * coalesced message request header.
			 */
			cmsg_reqhdr->size < info.max_pkt_size &&
			cmsg_reqhdr->size % sizeof(uint64_t) == 0);

		offset += sizeof(rpc_cmsg_reqhdr_t) + cmsg_reqhdr->size;
	}

	rpc_dassert(offset <= info.max_pkt_size);
}

// Accessors
coro_id_t* Rpc::get_next_coro_arr()
{
	return next_coro;
}

int Rpc::get_num_workers()
{
	rpc_dassert(info.num_workers >= 1 && info.num_workers <= HOTS_MAX_WORKERS);
	return info.num_workers;
}

int Rpc::get_wrkr_gid()
{
	rpc_dassert(info.wrkr_gid >= 0 && info.wrkr_gid < HOTS_MAX_WORKERS);
	return info.wrkr_gid;
}

int Rpc::get_wrkr_lid()
{
	rpc_dassert(info.wrkr_lid >= 0 && info.wrkr_lid < HOTS_MAX_SERVER_THREADS);
	return info.wrkr_lid;
}

size_t Rpc::get_stat_num_reqs()
{
	size_t ret = stat_num_reqs;
	stat_num_reqs = 0;
	return ret;
}

size_t Rpc::get_stat_num_creqs()
{
	size_t ret = stat_num_creqs;
	stat_num_creqs = 0;
	return ret;
}

/* Get the maximum batch latency (us) of all coroutines of this RPC endpoint */
double Rpc::get_max_batch_latency_us()
{
#if RPC_MSR_MAX_BATCH_LATENCY == 0
	return -1.0;
#else
	double max_lat_thread = 0;
	for(size_t i = 1; i < (unsigned) info.num_coro; i++) {
		if(max_batch_lat_coro[i] > max_lat_thread) {
			max_lat_thread = max_batch_lat_coro[i];
		}
	}

	return max_lat_thread;
#endif
}

/* Reset max latency tracking information */
void Rpc::reset_max_batch_latency()
{
#if RPC_MSR_MAX_BATCH_LATENCY == 0
	return;
#else
	for(size_t i = 1; i < (unsigned) info.num_coro; i++) {
		max_batch_lat_coro[i] = 0;
	}
#endif
}

Rpc::~Rpc()
{
	hrd_red_printf("Rpc: Destroying for worker %d\n", info.wrkr_gid);
	hrd_ctrl_blk_destroy(cb);	/* Destroy the control block */
}
