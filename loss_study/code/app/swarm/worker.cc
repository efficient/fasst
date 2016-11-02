#include "main.h"
#include "hrd.h"

/* TODO: Do we need to align these to avoid false sharing? */

/* Avoid posting RECVs after every poll_cq() */
__thread int recv_slack, recvs_to_post[MAX_PORTS] = {0};
__thread uint64_t seed;
__thread int active_qp[MAX_PORTS] = {0};
__thread int wrkr_gid; /* Global ID of this worker */
__thread int num_ports; /* Total ports to use */
__thread int num_threads; /* Worker threads on each machine */
__thread int pr_port;	/* Virtual primary port = wrkr_gid % num_ports */
__thread int postlist;
__thread int size_req;
__thread int size_resp;

/* For remote QPs */
__thread struct hrd_qp_attr *rem_wrkr_qp[NUM_WORKERS][MAX_PORTS];	/* Cold */
__thread struct ibv_ah *ah[NUM_WORKERS][MAX_PORTS];
__thread int rem_qpn[NUM_WORKERS][MAX_PORTS];
__thread int credits[NUM_WORKERS];
__thread long long last_req_to[NUM_WORKERS];
__thread long long last_resp_from[NUM_WORKERS];

/* For send() and recv(). Note for sizing @wc: HRD_RQ_DEPTH > MAX_POSTLIST. */
__thread struct hrd_ctrl_blk *cb[MAX_PORTS];
__thread long long nb_tx[MAX_PORTS][NUM_UD_QPS] = {{0}};
__thread struct ibv_send_wr send_wr[MAX_POSTLIST + 1];
__thread struct ibv_sge send_sgl[MAX_POSTLIST];
__thread struct ibv_recv_wr recv_wr[MAX_PORTS][HRD_RQ_DEPTH + 1];
__thread struct ibv_sge recv_sgl[MAX_PORTS][HRD_RQ_DEPTH + 1];
__thread struct ibv_wc wc[HRD_RQ_DEPTH];

/* Save the immediate payloads for request and response packets */
__thread uint32_t imm_req, imm_resp;

/* Stats: These get automatically initialized to 0. */
/* For average postlist size in response send()s */
__thread long long stat_post_send_calls;
__thread long long stat_sends_posted;

/* For average postlist size in recv()s */
__thread long long stat_post_recv_calls;
__thread long long stat_recvs_posted;

__thread long long tot_cycles_poll_recv_cq = 0;
__thread long long tot_cycles_post_recv = 0;
__thread long long tot_cycles_send_reqs = 0;
__thread long long tot_cycles_send_resps = 0;
__thread long long tot_cycles_send_resps_2 = 0;

/*
 * @stat_wasted_poll_cq is used at two places:
 * 1. For measuring time wasted in poll_cq()s that result in 0 completions.
 * 2. To determine when nodes stop processing more requests without any lost
 *    packets
 */
__thread long long stat_wasted_poll_cq;
#define NO_POLL_CQ_THRESH (10000000000ll)	/* 10 billion */

__thread FILE *out_fp;	/* File to record packet loss / swarm progress */
__thread bool pkt_loss_flag = false;
__thread int local_lid;

/* Enable debugging and stats per C file */
#define DEBUG_PRINTF 0
#define DEBUG_ASSERT 1
#define COLLECT_STATS 0
#define COLLECT_TIMER_INFO 0

#define debug_printf(fmt, ...) \
	do { if (DEBUG_PRINTF) fprintf(stderr, fmt, __VA_ARGS__); } while (0)
#define debug_assert(x) \
	do { if (DEBUG_ASSERT) assert(x); } while (0)
#define stat_inc(x, y) \
	do {if (COLLECT_STATS) x += y;} while (0)

#if COLLECT_TIMER_INFO == 1
#define get_cycles() hrd_get_cycles()
#else
#define get_cycles() 0
#endif

#define MILLION 1000000
#define BILLION 1000000000

/*
 * Record the total throughput and number of requests issued by a machine into
 * a file. If there are reordered or lost packets, total_reqs_ever should be
 * passed as -1. Also record each thread's individual total reqs.
 * 
 * When we execute run-servers.sh remotely, we redirect the stdout to dev null,
 * so this is saved to a file.
 */
void record_total_reqs_ever(double total_tput,
	long long total_reqs_ever, struct thread_params *params)
{
	char timebuf[50];
	hrd_get_formatted_time(timebuf);

	if(total_reqs_ever != -1) {
		fprintf(out_fp, "Machine tput = %.1f M/s, total reqs = %.3f B, time = %s.\n",
			(float) total_tput / MILLION, (float) total_reqs_ever / BILLION, timebuf);

		fprintf(out_fp, "[wrkr_gid, tput (M/s), reqs_ever (B)]: ");
		for(int i = 0; i < num_threads; i++) {
			fprintf(out_fp, "[%d, %.2f, %.3f], ",
				wrkr_gid + i, params->tput[i] / MILLION,
				(double) params->reqs_ever[i] / BILLION);
		}
		fprintf(out_fp, "\n");
	} else {
		fprintf(out_fp, "Error at time time = %s\n", timebuf);
	}

	fflush(out_fp);
}

/*
 * Used when a worker does not get RECV completions for a "really" long time.
 * Prints out the credits that the worker has for each remote worker.
 */
void record_credits()
{
	char timebuf[50];
	hrd_get_formatted_time(timebuf);

	fprintf(out_fp, "Worker %d recording credits at time %s:\n",
		wrkr_gid, timebuf);

	int wn;
	for(wn = 0; wn < NUM_WORKERS; wn++) {
		/* Print credits for remote workers only */
		if(wn / num_threads != wrkr_gid / num_threads) {
			fprintf(out_fp, "\tCredits to worker %d (machine %d) = %d\n",
				wn, wn / num_threads, credits[wn]);
		}		
	}

	/* Sleep for a long time so we can attach gdb */
	//sleep(10);
}

/* Initialize unchanging fields of the send work reqests */
void init_send_wrs(void *send_buf)
{
	int wr_i;
	for(wr_i = 0; wr_i < MAX_POSTLIST; wr_i++) {
		send_wr[wr_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

		send_wr[wr_i].opcode = IBV_WR_SEND_WITH_IMM;
		send_wr[wr_i].num_sge = 1;
		send_wr[wr_i].sg_list = &send_sgl[wr_i];

		send_sgl[wr_i].addr = (uint64_t) (uintptr_t) send_buf;
	}
}

void init_recv_wrs(int num_ports)
{
	int port_i = 0, wr_i = 0;

	for(port_i = 0; port_i < num_ports; port_i++) {
		for(wr_i = 0; wr_i < HRD_RQ_DEPTH; wr_i++) {
			recv_sgl[port_i][wr_i].length = BUF_SIZE;
			recv_sgl[port_i][wr_i].lkey = cb[port_i]->dgram_buf_mr->lkey;
			recv_sgl[port_i][wr_i].addr = (uintptr_t) &cb[port_i]->dgram_buf[0];

			recv_wr[port_i][wr_i].sg_list = &recv_sgl[port_i][wr_i];
			recv_wr[port_i][wr_i].num_sge = 1;
			recv_wr[port_i][wr_i].next = &recv_wr[port_i][wr_i + 1];
		}
	}
}

void init_imm_payloads()
{
	struct imm_payload *req_imm_payload = (struct imm_payload *) &imm_req;
	req_imm_payload->pkt_type = PKT_TYPE_REQ;
	req_imm_payload->wrkr_gid = wrkr_gid;

	struct imm_payload *resp_imm_payload = (struct imm_payload *) &imm_resp;
	resp_imm_payload->pkt_type = PKT_TYPE_RESP;
	resp_imm_payload->wrkr_gid = wrkr_gid;
}

/*
 * Send @num_requests requests using @send_buf.
 * Requests are sent using the remote worker's primary port.
 */
inline void send_requests(int num_requests, void *send_buf)
{
	debug_assert(num_requests <= WINDOW_SIZE);

	int req_i, wr_i = 0;	/* For non-postlist requests, wr_i remains 0 */
	int nb_wr[MAX_PORTS] = {0};

#if USE_REQ_POSTLIST == 1
	struct ibv_send_wr *first_send_wr[MAX_PORTS], *last_send_wr[MAX_PORTS];
#endif

	struct ibv_send_wr *bad_send_wr;
	//printf("Sending %d requests\n", num_requests);

	for(req_i = 0; req_i < num_requests; req_i++) {
		debug_assert(wr_i < MAX_POSTLIST);

		/* Pick a remote worker for which we have credits */
		int wn = hrd_fastrand(&seed) & NUM_WORKERS_;
		//if(wn >= NUM_WORKERS || wn % 2 != wrkr_gid % 2 || credits[wn] == 0) {
		if(wn >= NUM_WORKERS || credits[wn] == 0) {
			req_i--;
			continue;
		}

		credits[wn]--;
		int wn_port = wn & (num_ports - 1);	/* The port to send the request on */
		int _qp = active_qp[wn_port];	/* Use the active QP for this port */

#if USE_REQ_POSTLIST == 1
		if(nb_wr[wn_port] == 0) {
			first_send_wr[wn_port] = &send_wr[wr_i];
			last_send_wr[wn_port] = &send_wr[wr_i];
		} else {
			last_send_wr[wn_port]->next = &send_wr[wr_i];
			last_send_wr[wn_port] = &send_wr[wr_i];
		}
#endif

		nb_wr[wn_port]++;

		send_wr[wr_i].wr.ud.ah = ah[wn][wn_port];
		send_wr[wr_i].wr.ud.remote_qpn = rem_qpn[wn][wn_port];

		/* Encode the immediate payload for request */
#if CHECK_PACKET_LOSS == 1
		struct imm_payload *p = (struct imm_payload *) &send_wr[wr_i].imm_data;
		p->wrkr_gid = (uint32_t) wrkr_gid;
		p->pkt_type = PKT_TYPE_REQ;
		p->token = last_req_to[wn] & TOKEN_MASK;
		last_req_to[wn]++;
#else
		send_wr[wr_i].imm_data = imm_req;
#endif
		send_sgl[wr_i].length = size_req;

		//printf("Sending token %u to worker %d in request\n", p->token, wn);

		send_wr[wr_i].send_flags = ((nb_tx[wn_port][_qp] & UNSIG_BATCH_) == 0) ?
			IBV_SEND_SIGNALED : 0;
		if((nb_tx[wn_port][_qp] & UNSIG_BATCH_) == UNSIG_BATCH_) {
			hrd_poll_cq(cb[wn_port]->dgram_send_cq[_qp], 1, wc);
		}
		nb_tx[wn_port][_qp]++;

		send_wr[wr_i].send_flags |= IBV_SEND_INLINE;

		debug_printf("Worker %d: Sending request to worker %d using QP {%d, %d}\n",
			wrkr_gid, wn, wn_port, _qp);

#if USE_REQ_POSTLIST == 0
		send_wr[wr_i].next = NULL;
		debug_assert(wr_i == 0);	/* wr_i changes for postlist case only */
		int ret = ibv_post_send(cb[wn_port]->dgram_qp[_qp],
			&send_wr[0], &bad_send_wr);
		CPE(ret, "ibv_post_send error", ret);
#else
		/* Postlist case */
		wr_i++;
		/* wr_i = total number of requests assembled */
		if(wr_i == postlist || req_i == num_requests - 1) {
			debug_assert(wr_i > 0); /* If not, how did we get here? */

			int port_i;
			for(port_i = 0; port_i < num_ports; port_i++) {
				if(nb_wr[port_i] == 0) {
					continue;
				}

				last_send_wr[port_i]->next = NULL;
				
				int _qp = active_qp[port_i];
				int ret = ibv_post_send(cb[port_i]->dgram_qp[_qp],
					first_send_wr[port_i], &bad_send_wr);
				CPE(ret, "ibv_post_send error", ret);

				/*
				 * The wr's going to @port_i were signaled or unsignaled based
				 * on @active_qp[@port_i], so change @active_qp[@port_i] only
				 * after sending all resquests on @port_i.
				 */
				HRD_MOD_ADD(active_qp[port_i], NUM_UD_QPS);
				first_send_wr[port_i] = NULL;
				nb_wr[port_i] = 0;
			}

			wr_i = 0;	/* Reset to start of wr array */
			/* XXX: Should we calculate post_send() postlist stats here */
		}
#endif
	}

	//printf("Done posting %d requests\n", num_requests);
}

/* Post @num_recvs RECVs on the 0th QP of the control block on port @port_i */
// XXX: Move this to libhrd
inline void post_recvs(int port_i, int num_recvs)
{
	debug_assert(num_recvs > 0 && num_recvs < HRD_RQ_DEPTH);
	debug_assert(port_i >= 0 && port_i < num_ports);

	debug_printf("Worker %d: Posting %d RECVs on QP {%d, %d}\n",
		wrkr_gid, num_recvs, port_i, 0);

	int ret;
	struct ibv_recv_wr *bad_recv_wr;

	recv_wr[port_i][num_recvs - 1].next = NULL;

	ret = ibv_post_recv(cb[port_i]->dgram_qp[0],
		&recv_wr[port_i][0], &bad_recv_wr);
	CPE(ret, "ibv_post_recv error", ret);

	recv_wr[port_i][num_recvs - 1].next = &recv_wr[port_i][num_recvs];

	/* Stats */
	stat_inc(stat_recvs_posted, num_recvs);
	stat_inc(stat_post_recv_calls, 1);
}

/* Processes the available completions on each active port. */
inline int process_comps(void *send_buf)
{
	int port_i, comp_i, wr_i;	/* Iterators for port, completions, and wr's */
	int total_new_resps = 0;	/* Return value */
	struct ibv_send_wr *bad_send_wr;
	int _qp;

	for(port_i = 0; port_i < num_ports; port_i++) {
		long long poll_recv_cq_start = get_cycles();
		int num_comps = ibv_poll_cq(cb[port_i]->dgram_recv_cq[0],
			HRD_RQ_DEPTH, wc);	/* RECVs use QP #0 only */
		tot_cycles_poll_recv_cq += get_cycles() - poll_recv_cq_start;

		if(num_comps == 0) {
			stat_inc(stat_wasted_poll_cq, 1);
			continue;
		}

		recvs_to_post[port_i] += num_comps;
		if(recvs_to_post[port_i] >= recv_slack) {
			long long post_recv_start = get_cycles();
			post_recvs(port_i, recvs_to_post[port_i]);
			recvs_to_post[port_i] = 0;
			tot_cycles_post_recv += get_cycles() - post_recv_start;
		}

		long long send_resps_start_2 = get_cycles();

		wr_i = 0;	/* Start with the 0th work request */
		for(comp_i = 0; comp_i < num_comps; comp_i++) {
			struct imm_payload *imm_data =
				(struct imm_payload *) &wc[comp_i].imm_data;
			int pkt_type = imm_data->pkt_type;
			debug_assert(pkt_type == PKT_TYPE_REQ || pkt_type == PKT_TYPE_RESP);
			debug_assert(wc[comp_i].byte_len - 40 == (unsigned) size_resp);

			int wn = imm_data->wrkr_gid;
			debug_assert(wn >= 0 && wn <= NUM_WORKERS);

			if(pkt_type == PKT_TYPE_RESP) {
				debug_printf("Worker %d: Received response from worker %d on "
					"QP {%d, %d}\n", wrkr_gid, wn, port_i, 0);
				credits[wn]++;	/* We gain credits for more requests to @wn */
				total_new_resps++;

#if CHECK_PACKET_LOSS == 1
				/* Check for reordering */
				uint32_t token = imm_data->token;

				if(token != (last_resp_from[wn] & TOKEN_MASK)) {
					int remote_lid = wc[comp_i].slid;

					char timebuf[50];
					hrd_get_formatted_time(timebuf);

					fprintf(stderr, "Out-of-order response at "
						"worker %d (LID = %d or 0x%x), from "
						"worker %d (LID = %d or 0x%x) "
						"Token = %u, expected = %llu, TOKEN_MASK = %u, "
						"at time %s\n",
						wrkr_gid, local_lid, local_lid,
						wn, remote_lid, remote_lid,
						token, last_resp_from[wn] & TOKEN_MASK,
						TOKEN_MASK, timebuf);

					uint32_t expected = last_resp_from[wn] & TOKEN_MASK;
					uint32_t received = token;
					if(received != ((expected + 1) & TOKEN_MASK)) {
						fprintf(stderr, "Gap b/w expected and received >1 \n");
					}
					
					record_total_reqs_ever(0.0, -1, NULL);
					pkt_loss_flag = true;

					/* Make very sure that the error gets written out */
					fflush(out_fp);

					// Just make it right
					// XXX: Need to restrict all last_resp_from[] to TOKEN_MASK
					last_resp_from[wn] = token;
				}
				last_resp_from[wn]++;
#endif
				continue;
			}

			/* If here, this is a new request, so it should be on primary port */
			debug_assert(port_i == pr_port);
			debug_printf("Worker %d: Received request from worker %d on "
				"QP {%d, %d}\n", wrkr_gid, wn, pr_port, 0);

			_qp = active_qp[pr_port];
			
			send_wr[wr_i].wr.ud.ah = ah[wn][pr_port];
			send_wr[wr_i].wr.ud.remote_qpn = rem_qpn[wn][pr_port];

			send_wr[wr_i].next = &send_wr[wr_i + 1];	/* MAX_POSTLIST + 1 */

			/* Encode the immediate data for response */
#if CHECK_PACKET_LOSS == 1
			struct imm_payload *p = (struct imm_payload *)
				&send_wr[wr_i].imm_data;
			p->pkt_type = PKT_TYPE_RESP;
			p->wrkr_gid = (uint32_t) wrkr_gid;
			p->token = imm_data->token;	/* Copy the token back to response */
#else
			send_wr[wr_i].imm_data = imm_resp;
#endif
			send_sgl[wr_i].length = size_resp;

			send_wr[wr_i].send_flags =
				((nb_tx[pr_port][_qp] & UNSIG_BATCH_) == 0) ?
				IBV_SEND_SIGNALED : 0;
			if((nb_tx[pr_port][_qp] & UNSIG_BATCH_) == UNSIG_BATCH_) {
				hrd_poll_cq(cb[pr_port]->dgram_send_cq[_qp], 1, wc);
			}
			nb_tx[pr_port][_qp]++;

			send_wr[wr_i].send_flags |= IBV_SEND_INLINE;

			wr_i++;

			/* wr_i = total number of new responses assembled */
			if(wr_i == postlist) {
				send_wr[wr_i - 1].next = NULL;

				long long send_resps_start = get_cycles();
				int ret = ibv_post_send(cb[pr_port]->dgram_qp[_qp],
					&send_wr[0], &bad_send_wr);
				CPE(ret, "ibv_post_send error", ret);
				tot_cycles_send_resps += get_cycles() - send_resps_start;

				HRD_MOD_ADD(active_qp[pr_port], NUM_UD_QPS);
				wr_i = 0;	/* Reset to start a new postlist */

				/* Stats */
				stat_inc(stat_sends_posted, wr_i);
				stat_inc(stat_post_send_calls, 1);
			}
		}

		/* This block is required bc the above loop can end at 2 positions */
		if(wr_i != 0) {
			/*
			 * If some @send_wr's did not fit into a postlist in the loop above,
			 * then we should have updated _qp.
			 */
			debug_assert(_qp == active_qp[pr_port]);

			send_wr[wr_i - 1].next = NULL;

			long long send_resps_start = get_cycles();
			int ret = ibv_post_send(cb[pr_port]->dgram_qp[_qp],
				&send_wr[0], &bad_send_wr);
			CPE(ret, "ibv_post_send error", ret);
			tot_cycles_send_resps += get_cycles() - send_resps_start;

			HRD_MOD_ADD(active_qp[pr_port], NUM_UD_QPS);

			/* Stats */
			stat_inc(stat_sends_posted, wr_i);
			stat_inc(stat_post_send_calls, 1);
			/* No need to reset wr_i */
		}

		tot_cycles_send_resps_2 += get_cycles() - send_resps_start_2;
	}	/* End loop over ports */


	return total_new_resps;	/* Caller will send out new requests */
}

void *run_thread(void *arg)
{
	/*
	 * We should have enough RECVs for the following:
	 * 1. WINDOW_SIZE responses to our requests.
	 * 2. O(NUM_WORKERS * PER_WORKER_CREDITS) requests from remote threads.
	 */
	ct_assert(WINDOW_SIZE + (NUM_WORKERS * PER_WORKER_CREDITS) < HRD_RQ_DEPTH);

	/* Compute the available RECV slack and restrict it to MAX_POSTLIST */
	recv_slack = HRD_RQ_DEPTH -
		(WINDOW_SIZE + (NUM_WORKERS * PER_WORKER_CREDITS)) - 1;
	if(recv_slack >= MAX_POSTLIST) {
		recv_slack = MAX_POSTLIST;
	}

	int i, j;
	struct thread_params params = *(struct thread_params *) arg;

	/* Record the parameters that are needed in other functions */
	wrkr_gid = params.id;	/* Global ID of this thread */
	num_ports = params.num_ports;
	pr_port = wrkr_gid % num_ports;
	postlist = params.postlist;
	size_req = params.size_req;
	size_resp = params.size_resp;

	assert(num_ports <= MAX_PORTS);	/* Avoid dynamic alloc */

	num_threads = params.num_threads;
	int base_port_index = params.base_port_index;

	/* Create the output file */
	char filename[100];
	sprintf(filename, "losses/out-worker-%d", wrkr_gid);
	out_fp = fopen(filename, "w");
	assert(out_fp != NULL);

	for(i = 0; i < num_ports; i++) {
		int ib_port_index = base_port_index + i;

		cb[i] = hrd_ctrl_blk_init(wrkr_gid, /* local hid */
			ib_port_index, -1, /* port index, numa node */
			0, 0, /* #conn qps, uc */
			NULL, 0, -1, /* prealloc conn buf, buf size, key */
			NULL, NUM_UD_QPS, 4096, -1);	/* dgram qps, dgram buf size, key */

		/* Fill the 0th RECV queue */
		for(j = 0; j < HRD_RQ_DEPTH; j++) {
			hrd_post_dgram_recv(cb[i]->dgram_qp[0],
				(void *) cb[i]->dgram_buf, BUF_SIZE, cb[i]->dgram_buf_mr->lkey);
		}

		/* For each cb, publish 0th QP for remote workers to post SENDs to */
		char my_name[HRD_QP_NAME_SIZE];
		sprintf(my_name, "worker-%d-%d", wrkr_gid, i);
		hrd_publish_dgram_qp(cb[i], 0, my_name);
		printf("main: Worker %s published.\n", my_name);
	}

	/* Cache local LID to avoid re-probing it during packet loss */
	local_lid = hrd_get_local_lid(cb[0]->dgram_qp[0]->context,
		cb[0]->dev_port_id);

	/* Buffer to send responses from */
	int buf_size = size_resp > size_req ? size_resp : size_req;
	uint8_t *send_buf = (uint8_t *) malloc(buf_size + 1);	/* @buf_size can be 0 */
	assert(send_buf != 0);
	memset(send_buf, 0, buf_size);	/* The buffer's value doesn't matter */

	for(i = 0; i < NUM_WORKERS; i++) {
		for(j = 0; j < num_ports; j++) {
			char rem_wrkr_name[HRD_QP_NAME_SIZE];
			sprintf(rem_wrkr_name, "worker-%d-%d", i, j);

			rem_wrkr_qp[i][j] = NULL;
			ah[i][j] = NULL;
			while(rem_wrkr_qp[i][j] == NULL) {
				rem_wrkr_qp[i][j] = hrd_get_published_qp(rem_wrkr_name);
				if(rem_wrkr_qp[i][j] == NULL) {
					usleep(200000);
				}
			}

			/* Save the QPN */
			rem_qpn[i][j] = rem_wrkr_qp[i][j]->qpn;

			printf("main: Worker %d resolving %s (of %d workers).\n",
				wrkr_gid, rem_wrkr_name, NUM_WORKERS);
		
			struct ibv_ah_attr ah_attr;
			memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
			ah_attr.is_global = 0;
			ah_attr.dlid = rem_wrkr_qp[i][j]->lid;
			ah_attr.sl = 0;
			ah_attr.src_path_bits = 0;
			ah_attr.port_num = cb[j]->dev_port_id;	/* Local port */

			ah[i][j] = ibv_create_ah(cb[j]->pd, &ah_attr);
			assert(ah[i][j] != NULL);
		}
	}

	/* Initialize the RNG for this workers */
	seed = 0xdeadbeef;
	for(i = 0; i < wrkr_gid * 10000000; i++) {
		hrd_fastrand(&seed);
	}

	/* Initialize credits. 0 credits for workers on the same machine. */
	int total_credits = 0;
	for(i = 0; i < NUM_WORKERS; i++) {
		credits[i] = PER_WORKER_CREDITS;
		if(i / num_threads == wrkr_gid / num_threads) {
			credits[i] = 0;
		}

		total_credits += credits[i];
	}
	assert(total_credits >= WINDOW_SIZE);	/* Need credits to post a window */

	/* Initialize constant fields of the work requests */
	init_send_wrs(send_buf);
	init_recv_wrs(num_ports);

	/* Initialize the immediate payloads */
	init_imm_payloads();

	/* Start by sending out @WINDOW_SIZE requests */
	send_requests(WINDOW_SIZE, send_buf);

	long long nb_reqs_comp = 0;	/* For performance measurement */
	int nb_reqs_can_send = 0;	/* Number of requests we can send */

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	long long msr_start_cycles __attribute__((unused)) = get_cycles();

	while(1) {
		if(nb_reqs_comp >= M_2) {
			/* Measurement begin */
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) + 
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

#if COLLECT_STATS == 1 && COLLECT_TIMER_INFO == 1
			long long msr_end_cycles = get_cycles();
			long long ns = (end.tv_sec - start.tv_sec) * 1000000000 + 
				(end.tv_nsec - start.tv_nsec);

			printf("main: Server %d: ns = %lld, cycles = %lldM. "
				"tot_cycles_send_reqs = %lldM, tot_cycles_send_resps = {%lldM, %lldM} "
				"tot_cycles_post_recv = %lldM, tot_cycles_poll_recv_cq = %lldM\n",
				wrkr_gid, ns, (msr_end_cycles - msr_start_cycles) / 1000000,
				tot_cycles_send_reqs / 1000000,
				tot_cycles_send_resps / 1000000,
				tot_cycles_send_resps_2 / 1000000,
				tot_cycles_post_recv / 1000000,
				tot_cycles_poll_recv_cq / 1000000);

			printf("main: Server %d: %.2f requests/sec, "
				"Average SENDs per post_send() = %.2f, "
				"Average RECVs per post_recv() = %.2f, "
				"Requests completed = %lld, wasted poll_cq()s = %lld.\n",
				wrkr_gid, nb_reqs_comp / seconds,
				(double) stat_sends_posted / stat_post_send_calls,
				(double) stat_recvs_posted / stat_post_recv_calls,
				nb_reqs_comp, stat_wasted_poll_cq);
#else
			// Reduce output amount
			//char timebuf[50];
			//hrd_get_formatted_time(timebuf);
			//
			//printf("main: Worker %d: %.2f requests/sec. Time = %s\n",
			//	wrkr_gid, nb_reqs_comp / seconds, timebuf);
#endif
				
			params.tput[wrkr_gid % num_threads] = nb_reqs_comp / seconds;
			params.reqs_ever[wrkr_gid % num_threads] += nb_reqs_comp;
			if(wrkr_gid % num_threads == 0) {
				double total_tput = 0;
				long long total_reqs_ever = 0;
				for(i = 0; i < num_threads; i++) {
					total_tput += params.tput[i];
					total_reqs_ever += params.reqs_ever[i];
				}

				printf("main: Total tput %.2f M/s. Total reqs %.3f B\n",
					(float) total_tput / MILLION, (float) total_reqs_ever / BILLION);

				record_total_reqs_ever(total_tput, total_reqs_ever, &params);
			}
		
			/* Zero out the stats */
			stat_post_send_calls = 0;
			stat_sends_posted = 0;
			stat_post_recv_calls = 0;
			stat_recvs_posted = 0;
			stat_wasted_poll_cq = 0;

			/* Zero out perf counters */
			tot_cycles_poll_recv_cq = 0;
			tot_cycles_post_recv = 0;
			tot_cycles_send_reqs = 0;
			tot_cycles_send_resps = 0;
			tot_cycles_send_resps_2 = 0;

			msr_start_cycles = get_cycles();

			/* Reset measurement */
			nb_reqs_comp = 0;
			clock_gettime(CLOCK_REALTIME, &start);
		}

		/*
		 * If we do not get a completion for a very long time and didn't get any
		 * packet losses, it might mean that we lost our whole window of packets
		 * to a remote worker. Examining the credits can help in this case.
		 */
		if(unlikely(stat_wasted_poll_cq > NO_POLL_CQ_THRESH)) {
			record_credits();
			stat_wasted_poll_cq = 0;
		}

		/* The main loop */
		int nb_new_resps = process_comps(send_buf);

		nb_reqs_comp += nb_new_resps;
		nb_reqs_can_send += nb_new_resps;

		/* Send out a batch of requests after collecting a batch of responses */
		if(nb_reqs_can_send >= REQ_BATCH_SIZE) {
			if(pkt_loss_flag) {
				pkt_loss_flag = false;
				fprintf(stderr, "Worker %d: Received full batch after pkt loss\n",
					wrkr_gid);
			}

			long long send_reqs_start = get_cycles();
			send_requests(nb_reqs_can_send, send_buf);
			long long send_reqs_end = get_cycles();
			tot_cycles_send_reqs += (send_reqs_end - send_reqs_start);

			nb_reqs_can_send = 0;
		}
	}

	return NULL;
}
