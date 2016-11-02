#ifndef TX_EXECUTE_H
#define TX_EXECUTE_H

/* Read keys */
forceinline tx_status_t Tx::do_read(coro_yield_t &yield)
{
	tx_dassert(tx_status == tx_status_t::in_progress);

	tx_dassert(read_set.size() + write_set.size() <= RPC_MAX_MSG_CORO);
	tx_dassert(rs_index <= read_set.size());
	tx_dassert(ws_index <= write_set.size());

#if TX_ENABLE_LOCK_SERVER == 1
	if(mappings->use_lock_server) {
		tx_stat_inc(stat_lockserver_lock_req, 1);
		bool lock_success = send_lockserver_req(yield,
			locksrv_reqtype_t::lock);
		if(!lock_success) {
			tx_status = tx_status_t::must_abort;
			return tx_status;
		} else {
			tx_stat_inc(stat_lockserver_lock_req_success, 1);
			/* Record so we know whether to unlock on abort */
			lockserver_locked = true;
		}
	}
#endif

	rpc->clear_req_batch(coro_id);
	size_t req_i = 0;	/* Separate index bc we'll fetch both read, write set */
	
	/* Read the read set */
	for(size_t i = rs_index; i < read_set.size(); i++) {
		tx_rwset_item_t &item = read_set[i];

		rpc_req_t *req = rpc->start_new_req(coro_id,
			item.rpc_reqtype, item.primary_mn,
			(uint8_t *) &item.obj->hdr, sizeof(hots_obj_t));

		tx_req_arr[req_i] = req;
		req_i++;

		size_t size_req = ds_forge_generic_get_req(req, caller_id,
			item.key, item.keyhash, ds_reqtype_t::get_rdonly);
		req->freeze(size_req);
	}

	/* Read + lock the write set */
	for(size_t i = ws_index; i < write_set.size(); i++) {
		tx_rwset_item_t &item = write_set[i];

		rpc_req_t *req = rpc->start_new_req(coro_id,
			item.rpc_reqtype, item.primary_mn,
			(uint8_t *) &item.obj->hdr, sizeof(hots_obj_t));

		tx_req_arr[req_i] = req;
		req_i++;

		size_t size_req;
		/* In the execute phase, update and delete keys are handled similarly */
		if(item.write_mode != tx_write_mode_t::insert) {
			/* Update or delete */
			size_req = ds_forge_generic_get_req(req, caller_id,
				item.key, item.keyhash, ds_reqtype_t::get_for_upd);
		} else {
			/* Insert */
			size_req = ds_forge_generic_get_req(req, caller_id,
				item.key, item.keyhash, ds_reqtype_t::lock_for_ins);
		}
		
		req->freeze(size_req);
	}

	tx_dassert(req_i > 0 && req_i <= RPC_MAX_MSG_CORO);

	rpc->send_reqs(coro_id);
	tx_yield(yield);

	req_i = 0;

	/*
	 * a. Sanity-check the response.
	 * b. Record read set versions for validation.
	 * c. Record locking status of all write set keys to unlock on abort.
	 */
	for(size_t i = rs_index; i < read_set.size(); i++) {
		tx_rwset_item_t &item = read_set[i];
		ds_resptype_t resp_type = (ds_resptype_t) tx_req_arr[req_i]->resp_type;

		/* Hdr for successfully read keys need not be locked (bkt collison) */
		switch(resp_type) {
			case ds_resptype_t::get_rdonly_success:
				/* Response contains header and value */
				item.obj->val_size =
					tx_req_arr[req_i]->resp_len - sizeof(hots_hdr_t);
				check_item(item);	/* Checks @val_size */

				/* Save fields needed for validation */
				item.exec_rs_exists = true;
				item.exec_rs_version = item.obj->hdr.version;
				break;
			case ds_resptype_t::get_rdonly_not_found:
				/* Txn need not be aborted if a rdonly key is not found. */
				tx_dassert(tx_req_arr[req_i]->resp_len == sizeof(uint64_t));

				item.obj->val_size = 0;

				/* Save fields needed for validation */
				item.exec_rs_exists = false;
				item.exec_rs_version = item.obj->hdr.version;
				break;
			case ds_resptype_t::get_rdonly_locked:
				tx_dassert(tx_req_arr[req_i]->resp_len == 0);
				tx_status = tx_status_t::must_abort;
				break;
			default:
				printf("Tx: Unknown response type %u for read set key "
					"%" PRIu64 "\n.", tx_req_arr[req_i]->resp_type, item.key);
		}

		req_i++;
	}

	for(size_t i = ws_index; i < write_set.size(); i++) {
		tx_rwset_item_t &item = write_set[i];
		ds_resptype_t resp_type = (ds_resptype_t) tx_req_arr[req_i]->resp_type;

		if(item.write_mode != tx_write_mode_t::insert) {
			// Update or delete
			switch(resp_type) {
				case ds_resptype_t::get_for_upd_success:
					tx_dassert(item.obj->hdr.locked == 1);

					item.obj->val_size = 
						tx_req_arr[req_i]->resp_len - sizeof(hots_hdr_t);
					check_item(item); /* Checks @val_size */
			
					item.exec_ws_locked = true;	/* Mark for unlock on abort */
					break;
				case ds_resptype_t::get_for_upd_not_found:
				case ds_resptype_t::get_for_upd_locked:
					tx_dassert(tx_req_arr[req_i]->resp_len == 0);

					item.exec_ws_locked = false;	/* Don't unlock on abort */
					tx_status = tx_status_t::must_abort;
					break;
				default:
					printf("Tx: Unknown response type %u for write set "
						"(non-insert) key %" PRIu64 "\n.",
						tx_req_arr[req_i]->resp_type, item.key);
					exit(-1);
			}
		} else {
			// Insert
			switch(resp_type) {
				case ds_resptype_t::lock_for_ins_success:
					tx_dassert(item.obj->hdr.locked == 1);
					tx_dassert(tx_req_arr[req_i]->resp_len ==
						sizeof(hots_hdr_t));	/* Just the header */
					item.exec_ws_locked = true;	/* Mark for delete on abort */
					break;
				case ds_resptype_t::lock_for_ins_exists:
				case ds_resptype_t::lock_for_ins_locked:
					tx_dassert(tx_req_arr[req_i]->resp_len == 0);
					tx_status = tx_status_t::must_abort;
					item.exec_ws_locked = false; /* Don't unlock on abort */
					break;
				default:
					printf("Tx: Unknown response type %u for write set "
						"(insert) key %" PRIu64 "\n.",
						tx_req_arr[req_i]->resp_type, item.key);
					exit(-1);
			}
		}

		req_i++;
	}

	/*
	 * These indices only make sense if we return ex_success, so no need to
	 * update them in error cases.
	 */
	rs_index = read_set.size();
	ws_index = write_set.size();
	
	tx_dassert(tx_status == tx_status_t::in_progress ||
		tx_status == tx_status_t::must_abort);
	return tx_status;
}

#endif /* TX_EXECUTE_H */
