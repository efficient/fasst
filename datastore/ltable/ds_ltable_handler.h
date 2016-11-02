// RPC handler for an LTable datastore

#ifndef DS_LTABLE_HANDLER_H
#define DS_LTABLE_HANDLER_H

forceinline size_t ds_ltable_rpc_handler(
	uint8_t *resp_buf, rpc_resptype_t *resp_type,
	const uint8_t *req_buf, size_t req_len, void *_table)
{
	ds_dassert(resp_buf != NULL);
	ds_dassert(is_aligned(resp_buf, sizeof(uint64_t)));

	/* Sanity-check request buffer */
	ds_dassert(req_buf != NULL);
	ds_dassert(req_len >= sizeof(ds_generic_get_req_t)); /* get < put */
	ds_dassert(is_aligned(req_buf, sizeof(uint64_t)));

	ds_dassert(_table != NULL);

	LTable *table = static_cast<LTable *>(_table);
	MicaResult out_result;
	ds_reqtype_t req_type = static_cast<ds_reqtype_t>(req_buf[0]);

	switch(req_type) {

	/* All these requests require fetching the object first so group them. */
	case ds_reqtype_t::get_rdonly :
	case ds_reqtype_t::get_for_upd :
	case ds_reqtype_t::lock_for_ins : {
		ds_generic_get_req_t *req = (ds_generic_get_req_t *) req_buf;
		hots_key_t key = req->key;
		uint64_t keyhash = req->keyhash;
		size_t obj_size = 0;
		bool obj_found = false;

		/* Executing table->get will copy the HoTS object to resp_buf */
		out_result = table->get(keyhash, (char *) &key, sizeof(hots_key_t),
			(char *) resp_buf, sizeof(hots_obj_t), &obj_size, false);

		if(unlikely(out_result != MicaResult::kSuccess &&
			out_result != MicaResult::kNotFound)) {
			fprintf(stderr, "HoTS: Datastore get() for {table, key} = "
				"{%s, %" PRIu64 "} failed with unexpected code %s\n",
				table->name.c_str(), key,
				::mica::table::ResultString(out_result).c_str());
			exit(-1);
		}

		/* get_obj is only valid if the table->get() succeeded */
		hots_obj_t *get_obj = (hots_obj_t *) resp_buf;
		obj_found = out_result == MicaResult::kSuccess;

#if DS_DEBUG_ASSERT == 1
		if(obj_found) {
			/* Sanity check the object if we found it */
			ds_dassert(obj_size > 0 && obj_size <= sizeof(hots_obj_t));
			ds_dassert(hots_obj_size(get_obj->hdr.val_size) == obj_size);
			ds_dassert(get_obj->hdr.canary == HOTS_OBJHDR_CANARY);	/* Canary */
		}
#endif

		// Handle get_rdonly request
		if(req_type == ds_reqtype_t::get_rdonly) {
			if(!obj_found) {
				*resp_type = (uint16_t) ds_resptype_t::get_rdonly_not_found;
				return 0;
			} else {
				*resp_type = (uint16_t) ds_resptype_t::get_rdonly_success;
				return obj_size;
			}
		}

		// Handle get_for_upd request
		if(req_type == ds_reqtype_t::get_for_upd) {
			if(!obj_found) {
				*resp_type = (uint16_t) ds_resptype_t::get_for_upd_not_found;
				return 0;
			}

			if(get_obj->hdr.locked == 1) {
				*resp_type = (uint16_t) ds_resptype_t::get_for_upd_locked;
				return 0;
			}

			/* The object exists and is not locked. */
			uint64_t incremented_hdr = 0;
			out_result = table->increment(keyhash,
				(char *) &key, sizeof(hots_key_t), 1, &incremented_hdr);

			if(unlikely(out_result != MicaResult::kSuccess)) {
				fprintf(stderr, "HoTS: Datastore increment() for {table, key} = "
					"{%s, %" PRIu64 "} failed with code %s\n",
					table->name.c_str(), key,
					::mica::table::ResultString(out_result).c_str());
				exit(-1);
			}

			ds_dassert(incremented_hdr % 2 == 1);
			
			*resp_type = (uint16_t) ds_resptype_t::get_for_upd_success;
			return obj_size;
		}

		// Handle lock_for_ins request
		if(req_type == ds_reqtype_t::lock_for_ins) {
			if(obj_found) {
				*resp_type = (uint16_t) ds_resptype_t::lock_for_ins_exists;
				return 0;
			}
			
			/* Insert a pre-locked, temporary object into the datastore */
			hots_obj_t pl_obj;
			hots_format_tmp_objhdr(pl_obj);

			size_t pl_obj_size = hots_obj_size(pl_obj.hdr.val_size);
			out_result = table->set(keyhash, (char *) &key, sizeof(hots_key_t),
				(char *) &pl_obj, pl_obj_size, true);
			
			if(out_result != MicaResult::kSuccess) {
				fprintf(stderr, "HoTS: Failed to insert per-locked object into "
					"datastore for {%s, %" PRIu64 "}, code = %s\n",
					table->name.c_str(), key,
					::mica::table::ResultString(out_result).c_str());
				exit(-1);
			}

			*resp_type = (uint16_t) ds_resptype_t::lock_for_ins_success;
			return 0;
		}
	}

	case ds_reqtype_t::del : {
		ds_generic_get_req_t *req = (ds_generic_get_req_t *) req_buf;
		hots_key_t key = req->key;
		uint64_t keyhash = req->keyhash;

		out_result = table->del(keyhash, (char *) &key, sizeof(hots_key_t));

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

	/*
 	 * The object returned to the user in a the execute phase is an unlocked
	 * object. Writing this object to the datastore unlocks it automatically.
	 *
	 * The object timestamp must be set by the client.
	 */
	case ds_reqtype_t::put : {
		ds_generic_put_req_t *req = (ds_generic_put_req_t *) req_buf;
		hots_key_t key = req->key;
		uint64_t keyhash = req->keyhash;

		ds_dassert(req->obj.hdr.canary == HOTS_OBJHDR_CANARY);
		ds_dassert(req->obj.hdr.locked == 0);
		
		out_result = table->set(keyhash, (char *) &key, sizeof(hots_key_t),
			(char *) &req->obj, hots_obj_size(req->obj.hdr.val_size), true);

		if(unlikely(out_result != MicaResult::kSuccess)) {
			fprintf(stderr, "HoTS: Datastore put() for {table, key, obj_size} = "
				"{%s, %" PRIu64 ", %lu} failed with code %s\n",
				table->name.c_str(), key, hots_obj_size(req->obj.hdr.val_size),
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

#endif /* DS_LTABLE_HANDLER_H */
