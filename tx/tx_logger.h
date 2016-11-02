#ifndef TX_LOGGER_H
#define TX_LOGGER_H

forceinline bool Tx::log(coro_yield_t &yield)
{
	/* Tx::commit() for read-only txns should finish without logging */
	tx_dassert(write_set.size() >= 0);

	tx_dassert(RPC_MAX_MSG_CORO >= mappings->num_backups); /* 1 msg per backup */

	// Serialize the write set into the log record sent to all backups
	local_log_record->mchn_id = mappings->machine_id;
	local_log_record->coro_id = coro_id;
	local_log_record->num_keys = write_set.size();
	size_t req_len = sizeof(uint64_t);	/* Log record header */

	uint8_t *_buf = local_log_record->buf;

	for(size_t i = 0; i < write_set.size(); i++) {
		// Construct the log entry for this key. @item.obj contains a valid
		// HoTS object that has been checked using @check_item().
		tx_dassert(is_aligned(_buf, sizeof(uint64_t)));

		tx_rwset_item_t &item = write_set[i];

		/* Add the key */
		((uint64_t *) _buf)[0] = item.key;
		_buf += sizeof(uint64_t);
		req_len += sizeof(uint64_t);

		/* Append val size, header, and value by memcpy-ing the object */

		// XXX - Need to add req_type, and delete flag for deleted objects.
		// There is enough space in the object's @val_size field for this.
		size_t obj_size = hots_obj_size(item.obj->val_size);
			
		rte_memcpy((void *) _buf, &item.obj, obj_size);
		_buf += obj_size;
		req_len += obj_size;
	}

#if TX_DEBUG_ASSERT == 1
	local_log_record->magic = log_magic;
	local_log_record->debug_size = req_len;
#endif

	rpc->clear_req_batch(coro_id);

	uint64_t resp_buf;	/* Logger responses are 0-byte so we don't need them */
	rpc_req_t *rpc_req[HOTS_MAX_BACKUPS];

	for(int back_i = 0; back_i < mappings->num_backups; back_i++) {
		int log_mn = mappings->get_log_mn(back_i);

		rpc_req[back_i] = rpc->start_new_req(coro_id,
			RPC_LOGGER_REQ, log_mn,
			(uint8_t *) &resp_buf, sizeof(uint64_t));	/* Small resps */

		tx_dassert(rpc_req[back_i] != NULL && rpc_req[back_i]->req_buf != NULL);
		tx_dassert(is_aligned(rpc_req[back_i]->req_buf, sizeof(uint32_t)));

		rte_memcpy((void *) rpc_req[back_i]->req_buf,
			(void *) local_log_record, req_len);

		rpc_req[back_i]->freeze(req_len);
	}

	rpc->send_reqs(coro_id);
	tx_yield(yield);

	for(int back_i = 0; back_i < mappings->num_backups; back_i++) {
		logger_resptype_t resp_type =
			(logger_resptype_t ) rpc_req[back_i]->resp_type; _unused(resp_type);

		tx_dassert(resp_type == logger_resptype_t::success);
	}

	/* XXX: For now, logging always succeeds */
	return true;
}

#endif
