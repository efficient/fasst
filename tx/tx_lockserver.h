#ifndef TX_LOCKSERVER_H
#define TX_LOCKSERVER_H

/* Read keys */
forceinline bool Tx::send_lockserver_req(coro_yield_t &yield,
	locksrv_reqtype_t req_type)
{
	tx_dassert(mappings->use_lock_server);
	if(req_type == locksrv_reqtype_t::lock) {
		tx_dassert(tx_status == tx_status_t::in_progress);
	} else {
		tx_dassert(tx_status == tx_status_t::committed ||
			tx_status == tx_status_t::aborted);
	}

	tx_dassert(read_set.size() + write_set.size() <= RPC_MAX_MSG_CORO);

	rpc->clear_req_batch(coro_id);

	int ls_mn = mappings->get_lockserver_mn();
	
	/*
	 * Rpc expects a response buffer, although lockserver always sends a
	 * 0-length response.
	 */
	uint64_t resp_buf;
	rpc_req_t *rpc_req = rpc->start_new_req(coro_id,
		RPC_LOCKSERVER_REQ, ls_mn,
		(uint8_t *) &resp_buf, sizeof(uint64_t));

	tx_dassert(rpc_req != NULL && rpc_req->req_buf != NULL);
	tx_dassert(is_aligned(rpc_req->req_buf, sizeof(uint64_t)));

	size_t req_len = locksrv_req_size(read_set.size() + write_set.size());
	tx_dassert(rpc_req->available_bytes() >= req_len);

	locksrv_req_t *ls_req = (locksrv_req_t *) rpc_req->req_buf;
	ls_req->requester_id.wrkr_gid = mappings->wrkr_gid;
	ls_req->requester_id.coro_id = coro_id;

	ls_req->locksrv_reqtype = req_type;
	ls_req->num_keys = read_set.size() + write_set.size();

	size_t item_i = 0;	/* Separate index bc we'll lock both read, write set */
	
	/* Add the write set first to lock request */
	for(size_t i = 0; i < write_set.size(); i++) {
		tx_rwset_item_t &item = write_set[i];
		ls_req->key_arr[item_i].exclusive = 1;
		ls_req->key_arr[item_i].hashfrag = item.keyhash & locksrv_hashfrag_mask;
		item_i++;
	}

	/* Add the read set to lock request */
	for(size_t i = 0; i < read_set.size(); i++) {
		tx_rwset_item_t &item = read_set[i];
		ls_req->key_arr[item_i].exclusive = 0;
		ls_req->key_arr[item_i].hashfrag = item.keyhash & locksrv_hashfrag_mask;
		item_i++;
	}

	rpc_req->freeze(req_len);

	rpc->send_reqs(coro_id);
	tx_yield(yield);

	locksrv_resptype_t resp_type = (locksrv_resptype_t) rpc_req->resp_type;
	tx_dassert(resp_type == locksrv_resptype_t::success ||
		resp_type == locksrv_resptype_t::fail);

	if(req_type == locksrv_reqtype_t::lock) {
		return (resp_type == locksrv_resptype_t::success);
	} else {
		tx_dassert(resp_type == locksrv_resptype_t::success);
		return true;
	}
}

#endif /* TX_EXECUTE_H */
