#include <boost/coroutine/all.hpp>
#include <boost/bind.hpp>
#include <papi.h>

#include "main.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "hots.h"
#include "util/rte_memcpy.h"
#include "datastore/mica_ds.h"

/* Shared structures between master and slave coroutines */
__thread int wrkr_gid;	/* Global ID of this worker */
__thread int wrkr_lid;	/* Local ID of this worker */
__thread int num_coro;
__thread int num_keys;
__thread size_t val_size;
__thread int put_percentage;

__thread uint64_t seed = 0xdeadbeef;

__thread struct timespec msr_start, msr_end;

/* High-level HoTS structures */
__thread coro_call_t *coro_arr;
__thread coro_id_t *next_coro;
__thread Rpc *rpc;
__thread MicaTable *kv;

__thread long long stat_modified = 0;	/* Objects modified since start-of-day */
__thread long long stat_locked = 0;	/* Locked objects fetched with get_lock */

__thread long long iter_total = 0;

void master_func(coro_yield_t &yield, int coro_id)
{
	assert(rpc != NULL);
	assert(coro_id == RPC_MASTER_CORO_ID);

	// Allow each slave to run once

	/* Modify RPC's next_coro structure for initialization */
	next_coro = rpc->get_next_coro_arr();
	for(int coro_i = 0; coro_i < num_coro; coro_i++) {
		next_coro[coro_i] = (coro_i == num_coro - 1) ? 0 : coro_i + 1;
	}

	yield(coro_arr[1]);

	// Loop
	while(1) {
		next_coro = rpc->poll_comps();
		coro_id_t next_coro_id = next_coro[RPC_MASTER_CORO_ID];

		if(next_coro_id != RPC_MASTER_CORO_ID) {
			yield(coro_arr[next_coro_id]);
		}
	}
}

void slave_func(coro_yield_t &yield, int coro_id)
{
	assert(coro_id != RPC_MASTER_CORO_ID);
	assert(rpc != NULL);
	
	/* Initialize constant fields of the object to PUT */
	hots_obj_t put_obj;
	hots_format_real_objhdr(put_obj, val_size);
	memset((void *) put_obj.val, (uint8_t) wrkr_gid, val_size);
	
	int wn = 0;	/* Remote worker for a request */
	uint64_t key_arr[REQ_BATCH_SIZE];
	rpc_req_t *req_arr[REQ_BATCH_SIZE] __attribute__((unused));
	hots_obj_t obj_arr[REQ_BATCH_SIZE];	/* Output from tx/ */
	bool was_get[REQ_BATCH_SIZE] __attribute__((unused));

	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {
		rpc->clear_req_batch(coro_id);

		for(int i = 0; i < REQ_BATCH_SIZE; i++) {
			/* Choose a worker to send a message to */
#if USE_UNIQUE_WORKERS == 0
			wn = hrd_fastrand(&seed) & NUM_WORKERS_;
			while(wn >= NUM_WORKERS) {
				wn = hrd_fastrand(&seed) & NUM_WORKERS_;
			}
#else
			HRD_MOD_ADD(wn, NUM_WORKERS);
#if USE_SAME_PORT_WORKERS == 1
			while(wn % 2 != wrkr_gid % 2) {
				HRD_MOD_ADD(wn, NUM_WORKERS);
			}
#endif
#endif

			rpc_req_t *req = rpc->start_new_req(coro_id,
				RPC_MICA_REQ, wn, (uint8_t *) &obj_arr[i], sizeof(hots_obj_t));
			req_arr[i] = req;

			/* Choose a key */
			key_arr[i] = hrd_fastrand(&seed) & (num_keys - 1);
			uint64_t keyhash = hm_keyhash(key_arr[i]);

			/* Choose GET or PUT */
			size_t size_req;
			if(hrd_fastrand(&seed) % 100 < (uint64_t) put_percentage) {
				was_get[i] = false;
				size_req = hm_forge_generic_put_req(req,
					key_arr[i], keyhash, &put_obj, ds_reqtype_t::put);
			} else {
				was_get[i] = true;
#if USE_GET_LOCK == 1
				size_req = hm_forge_generic_get_req(req,
					key_arr[i], keyhash, ds_reqtype_t::get_for_upd);
#else
				size_req = hm_forge_generic_get_req(req,
					key_arr[i], keyhash, ds_reqtype_t::get_rdonly);
#endif
			}

			req->freeze(size_req);
		}

		rpc->send_reqs(coro_id);
		yield(coro_arr[next_coro[coro_id]]);

#if CHECK_RESP == 1
		/* Check the response */
		for(int i = 0; i < REQ_BATCH_SIZE; i++) {
			if(was_get[i] && req_arr[i]->resp_len > 0) {
				assert(req_arr[i]->resp_len == hots_obj_size(val_size));
				assert(obj_arr[i].hdr.val_size == val_size);
				assert(obj_arr[i].hdr.canary == HOTS_OBJHDR_CANARY);

				assert(obj_arr[i].val[0] == INITIAL_VALUE ||
					(obj_arr[i].val[0] < NUM_WORKERS));

				if(obj_arr[i].val[0] != INITIAL_VALUE) {
					int obj_creator_wn = (int) obj_arr[i].val[0];
					_unused(obj_creator_wn);
					assert(obj_creator_wn >= 0 && obj_creator_wn < NUM_WORKERS);
					stat_modified++;
				}
			} else if (was_get[i] && req_arr[i]->resp_len == 0) {
				stat_locked++;	/* The object was locked */
			} else {
				assert(!was_get[i]);
				assert(req_arr[i]->resp_len == 0);
			}
		}
#endif

		iter_total++;
		
		/* Any coroutine can print the measurement results */
		if((iter_total & K_256_) == K_256_) {
			clock_gettime(CLOCK_REALTIME, &msr_end);
			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;

			int total_reqs = iter_total * REQ_BATCH_SIZE;
			double get_fraction = (100.0 - put_percentage) / 100.0;

			printf("Worker %d: requests/s = %.2f M. "
				"Non-initial GET fraction = %.3f. Locked fraction = %.2f\n",
				wrkr_lid, total_reqs / msr_usec,
				stat_modified / (total_reqs * get_fraction),
				stat_locked / (total_reqs * get_fraction));

			clock_gettime(CLOCK_REALTIME, &msr_start);

			rpc->print_stats();
			iter_total = 0;
			stat_modified = 0;
			stat_locked = 0;
		}
	}
}

void run_thread(struct thread_params *params)
{
	/* Check if it is safe to use unique workers */
	if(USE_UNIQUE_WORKERS == 1) {
		assert(NUM_WORKERS >= REQ_BATCH_SIZE);	/* Else no unique batch exists */
	}

	/* The initial value in the datastore should ideally not be a wrkr_gid */
	assert(INITIAL_VALUE > NUM_WORKERS);

	wrkr_gid = params->id;	/* Global ID of this thread; TLS */
	wrkr_lid = wrkr_gid % params->num_threads;	/* Local ID of thread; TLS */
	num_coro = params->num_coro;	/* TLS */
	num_keys = params->num_keys;	/* TLS */
	val_size = params->val_size;	/* TLS */
	put_percentage = params->put_percentage;	/* TLS */

	/* Shift fastrand for each worker to avoid cache-friedliness */
	for(size_t i = 0; i < (size_t) wrkr_gid * 10000000; i++) {
		hrd_fastrand(&seed);
	}

	printf("Worker %d: starting.\n", wrkr_gid);

	/* Initialize KV */
	printf("Worker %d: initializing MICA. num_keys = %d\n", wrkr_gid, num_keys);
	kv = hm_init("kv.json",
		wrkr_lid, DIST_KV_BASE_SHM_KEY, DIST_KV_MAX_SHM_KEY);

	/* Pass -1 as num_workers to add all keys in {1, ..., num_keys} to kv */
	hm_populate(kv, num_keys, val_size, wrkr_gid, -1, INITIAL_VALUE);

	/* XXX: Computation of max_pkt_size belongs in the RPC layer */
	int max_pkt_size = (hm_put_req_size(val_size) +
		sizeof(rpc_cmsg_reqhdr_t)) * REQ_BATCH_SIZE;
	/* Initialize RPC. The RPC constructor will sanity-check args. */
	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		NUM_WORKERS, num_coro,
		params->base_port_index, params->num_ports,
		params->num_qps, params->numa_node, params->postlist,
		max_pkt_size);

	rpc = new Rpc(_rpc_args);
	rpc->register_rpc_handler(RPC_MICA_REQ, hm_rpc_handler, (void *) kv);

	/* Initialize coroutines */
	coro_arr = new coro_call_t[num_coro];
	for(int coro_i = 0; coro_i < num_coro; coro_i++) {
		if(coro_i == RPC_MASTER_CORO_ID) {
			coro_arr[coro_i] = coro_call_t(bind(master_func, _1, coro_i),
				attributes(fpu_not_preserved));
		} else {
			coro_arr[coro_i] = coro_call_t(bind(slave_func, _1, coro_i),
				attributes(fpu_not_preserved));
		}
	}

	/* Launch the master coroutine */
	coro_arr[RPC_MASTER_CORO_ID]();

	/* Master coroutine returns after @TOTAL_ITERS request batches */
	delete rpc;
}
