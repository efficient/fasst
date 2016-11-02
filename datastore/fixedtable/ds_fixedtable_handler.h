// RPC handler for a FixedTable datastore

#ifndef DS_FIXEDTABLE_HANDLER_H
#define DS_FIXEDTABLE_HANDLER_H

forceinline size_t ds_fixedtable_rpc_handler(
	uint8_t *resp_buf, rpc_resptype_t *resp_type,
	const uint8_t *req_buf, size_t req_len, void *_table)
{
	/* Sanity checks */
	ds_dassert(resp_buf != NULL);
	ds_dassert(is_aligned(resp_buf, sizeof(uint32_t)));

	ds_dassert(req_buf != NULL);
	ds_dassert(is_aligned(req_buf, sizeof(uint32_t)));

	ds_dassert(_table != NULL);

	MicaResult out_result;
	FixedTable *table = static_cast<FixedTable *>(_table);

	/* Extract caller ID, type, key and hash - works for both GETs and PUTs */
	ds_generic_get_req_t *req = (ds_generic_get_req_t *) req_buf;
	uint32_t caller_id = req->caller_id;
	ds_reqtype_t req_type = static_cast<ds_reqtype_t>(req->req_type);
	hots_key_t key = req->key;
	uint64_t keyhash = req->keyhash;

	/* Results will be copied to here */
	uint64_t *_hdr = (uint64_t *) resp_buf;
	char *_val_buf = (char *) resp_buf + sizeof(uint64_t);

	switch(req_type) {

	case ds_reqtype_t::get_rdonly : {
		ds_dassert(req_len == sizeof(ds_generic_get_req_t));
		out_result = table->get(caller_id, keyhash, key, _hdr, _val_buf);

		if(out_result == MicaResult::kSuccess) {
			ds_fixedtable_printf("DS FixedTable: get_rdonly request for "
				"key %lu. Success.\n", key);

			*resp_type = (uint16_t) ds_resptype_t::get_rdonly_success;
			return (sizeof(hots_hdr_t) + table->val_size); /* Header + value */
		} else if(out_result == MicaResult::kLocked) {
			ds_fixedtable_printf("DS FixedTable: get_rdonly request for "
				"key %lu. Failure = get_rdonly_locked.\n", key);
			*resp_type = (uint16_t) ds_resptype_t::get_rdonly_locked;
			return 0;	/* Must abort */
		} else {
			ds_dassert(out_result == MicaResult::kNotFound);
			ds_fixedtable_printf("DS FixedTable: get_rdonly request for "
				"key %lu. Failure = get_rdonly_not_found\n", key);
			*resp_type = (uint16_t) ds_resptype_t::get_rdonly_not_found;
			return sizeof(hots_hdr_t);	/* Only header; need not abort */
		}
	}

	case ds_reqtype_t::get_for_upd : {
		ds_dassert(req_len == sizeof(ds_generic_get_req_t));
		out_result = table->lock_bkt_and_get(caller_id, keyhash,
			key, _hdr, _val_buf);

		if(out_result == MicaResult::kSuccess) {
			ds_fixedtable_printf("DS FixedTable: get_for_upd request for "
				"key %lu. Success.\n", key);

			*resp_type = (uint16_t) ds_resptype_t::get_for_upd_success;
			return sizeof(hots_hdr_t) + table->val_size; /* Header + value */
		} else if (out_result == MicaResult::kLocked) {
			/* The object was locked */
			ds_fixedtable_printf("DS FixedTable: get_for_upd request for "
				"key %lu. Failure = get_for_upd_locked\n", key);
			*resp_type = (uint16_t) ds_resptype_t::get_for_upd_locked;
			return 0;	/* Must abort */
		} else {
			ds_dassert(out_result == MicaResult::kNotFound);
			/* The object did not exist */
			ds_fixedtable_printf("DS FixedTable: get_for_upd request for "
				"key %lu. Failure = get_for_upd_not_found\n", key);
			*resp_type = (uint16_t) ds_resptype_t::get_for_upd_not_found;
			return 0;	/* Must abort */
		}
	}

	case ds_reqtype_t::lock_for_ins : {
		ds_dassert(req_len == sizeof(ds_generic_get_req_t));

		out_result = table->lock_bkt_for_ins(caller_id, keyhash, key, _hdr);

		if(out_result == MicaResult::kSuccess) {
			ds_fixedtable_printf("DS FixedTable: lock_for_ins request for "
				"key %lu. Success.\n", key);
			*resp_type = (uint16_t) ds_resptype_t::lock_for_ins_success;
			return sizeof(hots_hdr_t); /* The coordinator needs the header */
		} else if(out_result == MicaResult::kExists) {
			/* The key already existed */
			ds_fixedtable_printf("DS FixedTable: lock_for_ins request for "
				"key %lu. Failure = lock_for_ins_exists\n", key);
			*resp_type = (uint16_t) ds_resptype_t::lock_for_ins_exists;
			return 0;	/* Must abort */
		} else {
			ds_dassert(out_result == MicaResult::kLocked);
			ds_fixedtable_printf("DS FixedTable: lock_for_ins request for "
				"key %lu. Failure = lock_for_ins_locked\n", key);
			*resp_type = (uint16_t) ds_resptype_t::lock_for_ins_locked;
			return 0;	/* Must abort */
		}
	}

	case ds_reqtype_t::unlock : {
		ds_dassert(req_len == sizeof(ds_generic_get_req_t));
		out_result = table->unlock_bucket_hash(caller_id, keyhash);

		if(unlikely(out_result != MicaResult::kSuccess)) {
			fprintf(stderr, "HoTS: Datastore unlock_bkt for {table, key} = "
				"{%s, %" PRIu64 "} failed with code %s\n",
				table->name.c_str(), key,
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}

		*resp_type = (uint16_t) ds_resptype_t::unlock_success;
		return 0;
	}

	case ds_reqtype_t::del : {
		ds_dassert(req_len == sizeof(ds_generic_get_req_t));
		out_result = table->del(caller_id, keyhash, key);

		/* del() returns kNotFound if we (wrongly) delete a non-existent key */
		if(unlikely(out_result != MicaResult::kSuccess)) {
			fprintf(stderr, "HoTS: Datastore del() for {table, key} = "
				"{%s, %" PRIu64 "} failed with code %s\n",
				table->name.c_str(), key,
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}

		*resp_type = (uint16_t) ds_resptype_t::del_success;
		return 0;
	}

	case ds_reqtype_t::put : {
		ds_dassert(req_len == ds_put_req_size(table->val_size));

		ds_generic_put_req_t *req = (ds_generic_put_req_t *) req_buf;
		ds_dassert(req->val_size == table->val_size);
		
		/* Only store the application-level opaque buffer. */
		out_result = table->set(caller_id, keyhash, key, (char *) &req->val);

		if(unlikely(out_result != MicaResult::kSuccess)) {
			fprintf(stderr, "HoTS: Datastore put() for {table, key, obj_size} = "
				"{%s, %" PRIu64 ", %lu} failed with code %s\n",
				table->name.c_str(), key, hots_obj_size(req->val_size),
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}

		*resp_type = (uint16_t) ds_resptype_t::put_success;
		return 0;
	}

	default: {
		fprintf(stderr, "HoTS: unknown datastore request type %u. Exiting.\n",
			(uint8_t) req_type);
		exit(-1);
	}
	}	/* End switch */
}

#endif /* DS_FIXEDTABLE_HANDLER_H */
