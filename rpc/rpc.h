#ifndef RPC_H
#define RPC_H

#include <list>

#include "libhrd/hrd.h"
#include "hots.h"
#include "rpc/rpc_types.h"
#include "rpc/rpc_defs.h"

#include "lockserver/lockserver.h"

/* For prefetching */
#include "datastore/fixedtable/ds_fixedtable.h"	/* Only prefetch FixedTable */

class Rpc {
private:
	struct rpc_args info;	/* Information about this RPC endpoint */

	// Handlers: An RPC handler writes its response data and the response type
	// to resp_buf and resp_type respectively, and returns the response length
	size_t (*rpc_handler[RPC_MAX_REQ_TYPE])(
		uint8_t *resp_buf, rpc_resptype_t *resp_type,
		const uint8_t *req_buf, size_t req_len, void *arg) =
		{NULL};
	void *rpc_handler_arg[RPC_MAX_REQ_TYPE] = {NULL};

	// Coroutine stuff
	rpc_req_batch_t req_batch_arr[RPC_MAX_CORO];	/* For slaves*/
	rpc_resp_batch_t resp_batch;	/* For master coroutine */
	coro_id_t next_coro[RPC_MAX_CORO];

	// Packet loss detection (ld)
	size_t ld_iters = 0;
	struct timespec ld_stopwatch; /* Counts RPC_LOSS_DETECTION_MS at runtime */
	size_t ld_num_resps_ever[RPC_MAX_CORO] = {0};
	size_t ld_num_resps_ever_prev[RPC_MAX_CORO] = {0};
	FILE *ld_fp; /* File to record any detected losses */
	struct timespec rpc_init_time;	/* Creation time of this RPC endpoint */

#if RPC_MSR_MAX_BATCH_LATENCY
	// Tracking information to measure max batch latency
	struct timespec max_batch_lat_start[RPC_MAX_CORO];
	double max_batch_lat_coro[RPC_MAX_CORO] = {0}; /* Max latency per coro */
#endif

	// RDMA stuff
	int pr_port;	/* Port used by this RPC endpoint. */
	struct hrd_ctrl_blk *cb = NULL;	/* A control block on @pr_port */
	uint32_t lkey;	/* Saved lkey of the primary port  */

	/* For remote QPs */
	struct ibv_ah *ah[HOTS_MAX_MACHINES] = {NULL};
	int rem_qpn[HOTS_MAX_MACHINES] = {0};

	// send()
	int active_qp = 0;	/* The active QP for post_send() */
	int nb_pending[RPC_MAX_QPS] = {0};	/* For selective signalling */
	struct ibv_send_wr send_wr[RPC_MAX_POSTLIST + 1]; /* +1 for blind ->next */
	struct ibv_sge send_sgl[RPC_MAX_POSTLIST];	/* No need for +1 here */

	// recv()
	size_t recv_step = -1;	/* Step size into cb->dgram_buf for RECV posting */
	int recv_head = 0; 	/* Current un-posted RECV buffer */
	int recv_slack = 0;
	int recvs_to_post = 0;

	/* Once post_recvs_fast() is used, regular post_recv() must not be used */
	bool fast_recv_used = false;

	struct ibv_recv_wr recv_wr[HRD_RQ_DEPTH];
	struct ibv_sge recv_sgl[HRD_RQ_DEPTH];
	struct ibv_wc wc[HRD_RQ_DEPTH];

	/* Lockserver */
	std::list<locksrv_req_t> locksrv_req_list;

	// Stats and cycle counts
	size_t stat_num_reqs; /* Number of NON-COALESCED reqs. Useful for apps. */
	size_t stat_num_creqs; /* Number of COALESCED reqs. Useful for apps. */

	/* For average postlist size in coalesced response send()s */
	size_t stat_resp_post_send_calls = 0;
	size_t stat_num_cresps = 0;	/* Number of COALESCED responses */

	size_t tot_cycles_poll_recv_cq = 0;
	size_t tot_cycles_post_recv = 0, stat_num_recvs = 0;

	size_t stat_wasted_poll_cq = 0;

public:
	Rpc(struct rpc_args);
	void register_rpc_handler(int req_type,
		size_t (*func)(uint8_t* resp_buf, rpc_resptype_t *resp_type,
			const uint8_t* req_buf, size_t req_len, void *arg),
		void *arg);
	int required_recvs();	/* Number of RECVs needed on each QP */
	~Rpc();

	// Datapath
	int send_reqs(int coro_id);	/* Used by slave coro to send queued requests */
	int send_resps();	/* Used by master coroutine to flush queued responses */
	coro_id_t* poll_comps();	/* Process RECVs; return completed coroutines */
	void ld_check_packet_loss();


	// Lockserver
	void locksrv_loop(Lockserver *lockserver);
	void locksrv_process_queue(Lockserver *lockserver);


	// Accessors / debug / stats
	int get_num_workers();
	int get_wrkr_gid();
	int get_wrkr_lid();
	coro_id_t *get_next_coro_arr();
	size_t get_stat_num_reqs();
	size_t get_stat_num_creqs();
	double get_max_batch_latency_us();
	void reset_max_batch_latency();
	void print_stats();

	/* Clear the current message batch for this coroutine. */
	forceinline void clear_req_batch(int coro_id)
	{
		rpc_dassert(coro_id >= 1 && coro_id < info.num_coro);

		rpc_req_batch_t *req_batch = &req_batch_arr[coro_id];
		req_batch->clear();
	}

	forceinline rpc_req_t* start_new_req(coro_id_t coro_id,
		rpc_reqtype_t req_type, int resp_mn, uint8_t *resp_buf,
		size_t max_resp_len)
	{
		rpc_dassert(coro_id >= 0 && coro_id < info.num_coro);
		rpc_dassert(req_type <= RPC_MAX_REQ_TYPE);
		rpc_dassert(resp_mn >= 0 && resp_mn < info.num_machines);
		rpc_dassert(resp_buf != NULL && is_aligned(resp_buf, 8));
		rpc_dassert(max_resp_len > 0);

		stat_num_reqs++;	/* Useful for apps so don't use rpc_stat_inc */

		rpc_req_batch_t *req_batch = &req_batch_arr[coro_id];
		rpc_dassert(req_batch->num_reqs_done == 0);	/* Sanity check */

		/* Start a fresh request */
		int _num_reqs = req_batch->num_reqs;
		rpc_dassert(_num_reqs < RPC_MAX_MSG_CORO);

		rpc_req_t *req = &req_batch->req_arr[_num_reqs];
		req->resp_buf = resp_buf;
		req->max_resp_len = max_resp_len;
		req_batch->num_reqs++;

		// Choose a coalesced message
		int cmsg_i = -1;
		if(req_batch->cmsg_for_mc[resp_mn] >= 0) {
			/* We have already started a coalesced message for this machine */
			cmsg_i = req_batch->cmsg_for_mc[resp_mn];
		} else {
			/* Start a fresh coalesced message */
			rpc_dassert(req_batch->num_uniq_mn < RPC_MAX_MSG_CORO);

			cmsg_i = req_batch->num_uniq_mn;
			req_batch->cmsg_for_mc[resp_mn] = cmsg_i; /* Record the used cmsg */
			req_batch->num_uniq_mn++;

			/* The fresh coalesced message must be fresh */
			rpc_cmsg_t *cmsg = &req_batch->cmsg_arr[cmsg_i];
			rpc_dassert(cmsg->remote_mn == RPC_INVALID_MN);
			rpc_dassert(cmsg->num_centry == 0);

			cmsg->remote_mn = resp_mn;
		}

		/* This logic is for both old and fresh coalesced messages */
		rpc_dassert(cmsg_i >= 0 && cmsg_i < RPC_MAX_MSG_CORO);
		rpc_cmsg_t *cmsg = &req_batch->cmsg_arr[cmsg_i];
		rpc_dassert(cmsg->num_centry < RPC_MAX_MSG_CORO);
		cmsg->num_centry++;	/* Increment requests in coalesced message */

		/* Sanity-check the chosen coalesced message */
		rpc_dassert(cmsg->req_mbuf.is_valid());
		rpc_dassert(cmsg->resp_mbuf.is_valid());

		/* Fill the coalesced message request header */
		rpc_dassert(cmsg->req_mbuf.available_bytes() >
			sizeof(rpc_cmsg_reqhdr_t));

		rpc_cmsg_reqhdr_t *cmsg_hdr =
			(rpc_cmsg_reqhdr_t *) cmsg->req_mbuf.cur_buf;
		cmsg_hdr->req_type = req_type;
		cmsg_hdr->coro_seqnum = req_batch->num_reqs - 1;	/* Already ++d */
		cmsg_hdr->magic = RPC_CMSG_REQ_HDR_MAGIC;	/* Debug only */
		cmsg->req_mbuf.cur_buf += sizeof(rpc_cmsg_reqhdr_t);

		/* Fill in the request's remaining fields */
		req->req_buf = cmsg->req_mbuf.cur_buf;	/* Given to the RPC user */
		req->_cmsg_reqhdr = cmsg_hdr;
		req->_cmsg_req_mbuf = &cmsg->req_mbuf;	/* For bookkeeping */

		rpc_dprintf("Rpc: Worker %d, coro %d created %s request for machine %d. "
			"Cmsg index = %d\n", info.wrkr_gid, coro_id,
			rpc_type_to_string(req_type).c_str(), resp_mn, cmsg_i);

		return req;
	}

	/*
	 * Start a new response for a machine that sent @num_reqs coalesced requests
	 * with immediate = @req_imm.
	 */
	forceinline hots_mbuf_t* start_new_resp(int req_mn,
		int num_reqs, uint32_t req_imm)
	{
		rpc_dassert(resp_batch.num_cresps < HRD_RQ_DEPTH);

		/* Copy over the request's immediate + modify some fields */
		union rpc_imm resp_imm;
		resp_imm.int_rep = req_imm;
		rpc_dassert(resp_imm.is_req == 1);
		resp_imm.is_req = 0;	/* Convert to response type */
		resp_imm.mchn_id = info.machine_id;

		/* Choose a fresh coalesced message */
		rpc_cmsg_t *cmsg = &resp_batch.cmsg_arr[resp_batch.num_cresps];
		cmsg->remote_mn = req_mn;
		cmsg->num_centry = num_reqs;
		cmsg->resp_imm = resp_imm.int_rep;

		resp_batch.num_cresps++;

		/* Sanity-check and return its response mbuf */
		hots_mbuf_t *resp_mbuf = &cmsg->resp_mbuf;

		rpc_dassert(resp_mbuf->is_valid());
		rpc_dassert(resp_mbuf->length() == 0);

		return resp_mbuf;
	}

	// Debug and statistics
	static void initialize_dummy_args(struct rpc_args *args);

private:
	/*
	 * Given a coalesced request with num_reqs items, prefetch the index for
	 * MICA requests. Works for both GETs and PUTs.
	 */
	inline void prefetch_mica(uint8_t *wc_buf, int num_reqs)
	{
		size_t wc_off = 0;	/* Offset into wc_buf */
		rpc_cmsg_reqhdr_t *cmsg_reqhdr;

		for(int i = 0; i < (int) num_reqs; i++) {
			/* Unmarshal the request header */
			cmsg_reqhdr = (rpc_cmsg_reqhdr_t *) &wc_buf[wc_off];
			rpc_dassert(cmsg_reqhdr->magic == RPC_CMSG_REQ_HDR_MAGIC);
			uint32_t req_type = cmsg_reqhdr->req_type;
			uint32_t req_len = cmsg_reqhdr->size;

			/* Special logic for prefetching MICA tables */
			if(req_type >= RPC_MICA_REQ_BASE) {
				ds_generic_get_req_t *req = (ds_generic_get_req_t *)
					&wc_buf[wc_off + sizeof(rpc_cmsg_reqhdr_t)];
				uint64_t keyhash = req->keyhash;
				FixedTable *table = (FixedTable *) rpc_handler_arg[req_type];
				table->prefetch_table(keyhash);
			}

			/* Move to next coalesced message unconditionally */
			wc_off += sizeof(rpc_cmsg_reqhdr_t) + req_len;
		}
	}

	void check_defines();
	void check_info();

	void check_imm(union rpc_imm imm);
	void check_coalesced_msg(uint8_t *cbuf, int num_centry);
	
	void init_non_zero_members();
	void init_coroutine_metadata();

	/* Initialize unchanging fields of wr's for performance */
	void init_send_wrs();
	void init_recv_wrs();
	void check_modded();

	/* Post @num_recvs RECVs on the 0th QP of the control block on @port_i */
	inline void post_recvs(int num_recvs)
	{
		rpc_dassert(fast_recv_used == false);

		/* The recvs posted are @first_wr through @last_wr, inclusive */
		struct ibv_recv_wr *first_wr, *last_wr, *temp_wr, *bad_wr;

		rpc_dassert(num_recvs > 0 && num_recvs <= HRD_RQ_DEPTH);

		rpc_dprintf("Worker %d: Posting %d RECVs on QP 0\n",
			info.wrkr_gid, num_recvs);

		int ret;
		int first_wr_i = recv_head;
		int last_wr_i = first_wr_i + num_recvs - 1;
		if(last_wr_i >= HRD_RQ_DEPTH) {
			last_wr_i -= HRD_RQ_DEPTH;
		}

		first_wr = &recv_wr[first_wr_i];
		last_wr = &recv_wr[last_wr_i];
		temp_wr = last_wr->next;
		last_wr->next = NULL;	/* Break circularity */

		ret = ibv_post_recv(cb->dgram_qp[0], first_wr, &bad_wr);
		CPE(ret, "Rpc: ibv_post_recv error", ret);
		last_wr->next = temp_wr;	/* Restore circularity */

		/* Update recv head: go to the last wr posted and take 1 more step */
		recv_head = last_wr_i;
		HRD_MOD_ADD(recv_head, HRD_RQ_DEPTH);	/* 1 step */
	}

	inline void post_recvs_fast(int num_recvs)
	{
		fast_recv_used = true;

		/*
		 * Construct a special RECV wr that the instrumented driver understands.
		 * Encode the number of required RECVs in its @num_sge field.
		 */
		struct ibv_recv_wr special_wr;
		special_wr.wr_id = 3185;
		special_wr.num_sge = num_recvs;

		struct ibv_recv_wr *bad_wr = &special_wr;

		int ret = ibv_post_recv(cb->dgram_qp[0], NULL, &bad_wr);
		CPE(ret, "Rpc: Fast post recv: ibv_post_recv error", ret);
	}

	/*
	 * 1. Compute work request flag based on QP window status and size.
	 * 2. If QP unsignaling window is full, poll the QP for 1 completion.
	 */
	forceinline uint32_t set_flags(int qp, size_t size)
	{
		uint32_t flag = (nb_pending[qp] == 0) ? IBV_SEND_SIGNALED : 0;

		flag |= (size <= HRD_MAX_INLINE ? IBV_SEND_INLINE : 0);

		if(nb_pending[qp] == RPC_UNSIG_BATCH - 1) {
			hrd_poll_cq(cb->dgram_send_cq[qp], 1, wc);
			nb_pending[qp] = 0;
		} else {
			nb_pending[qp]++;
		}

		return flag;
	}

};

#endif /* RPC_H */
