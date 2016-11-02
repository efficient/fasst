#include "rpc/rpc.h"
#include "util/rte_memcpy.h"

/* Lockserver code needs fixes for the move to CRCW mode */
#define RPC_LOCKSRV_ENABLE 0

void Rpc::locksrv_process_queue(Lockserver *lockserver)
{
	return;
#if RPC_LOCKSRV_ENABLE == 1
	if(locksrv_req_list.size() == 0) {
		return;
	}

	/* Bookkeeping */
	int wr_i = 0;
	int _qp = active_qp[pr_port];

	rpc_cmsg_reqhdr_t resp_cmsg_reqhdr[RPC_MAX_POSTLIST];

	for(auto it = locksrv_req_list.begin(); it != locksrv_req_list.end();) {
		auto ls_req = it;

		locksrv_reqtype_t req_type = ls_req->locksrv_reqtype;
		int num_keys = ls_req->num_keys;

		bool success = true;

		if(req_type == locksrv_reqtype_t::lock) {
			// Handle a lock request
			for(int i = 0; i < num_keys; i++) {
				bool ret = lockserver->lock(ls_req->requester_id,
					ls_req->key_arr[i].exclusive, ls_req->key_arr[i].hashfrag);
				if(!ret) {
					/* Unlock key i - 1 --> 0 (read set first) */
					for(int j = i - 1; j >= 0; j--) {
						lockserver->unlock(ls_req->requester_id,
							ls_req->key_arr[j].exclusive,
							ls_req->key_arr[j].hashfrag);
					}

					success = false;
					break;	/* Do not try to lock more keys */
				}
			}
		} else {
			// Handle an unlock request. Unlock read set first.
			for(int i = num_keys - 1; i >= 0; i--) {
				lockserver->unlock(ls_req->requester_id,
					ls_req->key_arr[i].exclusive, ls_req->key_arr[i].hashfrag);
			}
		}

		if(success) {
			rpc_dassert(wr_i < RPC_MAX_POSTLIST);

			/* Encode the response: XXX: We want to save this in the list. */
			resp_cmsg_reqhdr[wr_i].resp_type =
				(uint16_t) locksrv_resptype_t::success;
			resp_cmsg_reqhdr[wr_i].coro_seqnum = 0;
			resp_cmsg_reqhdr[wr_i].magic = RPC_CMSG_REQ_HDR_MAGIC;
			resp_cmsg_reqhdr[wr_i].size = 0;

			/* Verify constant fields */
			rpc_dassert(send_wr[wr_i].wr.ud.remote_qkey == HRD_DEFAULT_QKEY);
			rpc_dassert(send_wr[wr_i].opcode == IBV_WR_SEND_WITH_IMM);
			rpc_dassert(send_wr[wr_i].num_sge == 1);
			rpc_dassert(send_wr[wr_i].sg_list == &send_sgl[wr_i]);

			int req_wn = ls_req->requester_id.wrkr_gid;

			/* Encode variable fields */
			send_wr[wr_i].wr.ud.ah = ah[req_wn][pr_port];
			send_wr[wr_i].wr.ud.remote_qpn = rem_qpn[req_wn][pr_port];
			send_wr[wr_i].next = &send_wr[wr_i + 1];	/* Safe */

			send_sgl[wr_i].addr = (uintptr_t) &resp_cmsg_reqhdr[wr_i];
			send_sgl[wr_i].length = sizeof(rpc_cmsg_reqhdr_t);
			send_sgl[wr_i].lkey = lkey[pr_port];

			send_wr[wr_i].send_flags = set_flags(pr_port,
				_qp, send_sgl[wr_i].length);

			/* Encode the immediate. XXX: We want to save this in the list. */
			union rpc_imm resp_imm;
			resp_imm.imm.is_req = 0;
			resp_imm.imm.num_reqs = 1;
			resp_imm.imm.wrkr_gid = ls_req->requester_id.wrkr_gid;
			resp_imm.imm.coro_id = ls_req->requester_id.coro_id;

			send_wr[wr_i].imm_data = resp_imm.int_rep;
			wr_i++;
		}

		/* wr_i = total number of reqs. Check if we should send response. */

		auto final_it = locksrv_req_list.end();
		final_it--;

		if(wr_i == info.postlist || (it == final_it && wr_i != 0)) {
			rpc_dassert(wr_i > 0);

			send_wr[wr_i - 1].next = NULL;
			ibv_send_wr *bad_wr;
			
			int ret = ibv_post_send(cb[pr_port]->dgram_qp[_qp],
				&send_wr[0], &bad_wr);
			rpc_dassert_msg(ret == 0, "Rpc: ibv_post_send error\n");

			rpc_stat_inc(stat_resp_post_send_calls, 1);
			HRD_MOD_ADD(active_qp[pr_port], info.num_qps);
	
			/* Reset */
			wr_i = 0;
			_qp = active_qp[pr_port];
		}

		if(success) {
			it = locksrv_req_list.erase(it);
		} else {
			it++;
		}
	}

}

void Rpc::locksrv_loop(Lockserver *lockserver)
{
	while(1) {
	for(int port_i = 0; port_i < info.num_ports; port_i++) {
		locksrv_process_queue(lockserver);

		int cq_comps = ibv_poll_cq(cb[port_i]->dgram_recv_cq[0],
			HRD_RQ_DEPTH, wc);

		if(cq_comps == 0) {
			continue;
		}

		for(int comp_i = 0; comp_i < cq_comps; comp_i++) {
			/* Unmarshal the completion's immediate */
			union rpc_imm wc_imm;
			wc_imm.int_rep = wc[comp_i].imm_data;
			check_imm(wc_imm);;

			uint32_t _is_req __attribute__((unused)) = wc_imm.imm.is_req;
			uint32_t _num_reqs __attribute__((unused)) = wc_imm.imm.num_reqs;
			uint32_t _wrkr_gid __attribute__((unused)) = wc_imm.imm.wrkr_gid;
			uint32_t _coro_id __attribute__((unused)) = wc_imm.imm.coro_id;
			uint32_t _config_id __attribute__((unused)) = wc_imm.imm.config_id;

			rpc_dassert(_is_req == 1 && _num_reqs == 1);
			
			/* Interpret the received buffer */
			uint8_t *wc_buf = (uint8_t *) (wc[comp_i].wr_id + HOTS_GRH_BYTES);
			rpc_dassert(is_aligned(wc_buf, 64));

			/* wc.byte_len includes GRH, whether or not GRH is DMA-ed */
			size_t wc_len = wc[comp_i].byte_len - HOTS_GRH_BYTES;
			rpc_dassert(wc_len >= 0 && wc_len <= info.max_pkt_size);
			_unused(wc_len);

			/* Sanity-check the coalesced request message header */
			rpc_cmsg_reqhdr_t *cmsg_reqhdr = (rpc_cmsg_reqhdr_t *) wc_buf;
			rpc_dassert(cmsg_reqhdr->req_type == RPC_LOCKSERVER_REQ);
			rpc_dassert(cmsg_reqhdr->coro_seqnum == 0);
			rpc_dassert(cmsg_reqhdr->magic == RPC_CMSG_REQ_HDR_MAGIC);
			rpc_dassert(cmsg_reqhdr->size <= sizeof(locksrv_req_t));
			_unused(cmsg_reqhdr);

			locksrv_req_t *locksrv_req = (locksrv_req_t *) (wc_buf +
				sizeof(rpc_cmsg_reqhdr_t));
			rpc_dassert(locksrv_req->requester_id.wrkr_gid == _wrkr_gid &&
				locksrv_req->requester_id.coro_id == _coro_id);
			rpc_dassert(locksrv_req->locksrv_reqtype == locksrv_reqtype_t::lock
				|| locksrv_req->locksrv_reqtype == locksrv_reqtype_t::unlock);
			rpc_dassert(locksrv_req->num_keys > 0);

			locksrv_req_list.push_back(*locksrv_req);
		}

		/* Handle RECVs */
		recvs_to_post[port_i] += cq_comps;
		if(recvs_to_post[port_i] >= recv_slack) {
			post_recvs(port_i, recvs_to_post[port_i]);
			recvs_to_post[port_i] = 0;
		}

	}
	}	/* End while loop */
#endif
}
