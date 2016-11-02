#include "main.h"
#include "libhrd/hrd.h"
#include "rpc/rpc.h"
#include "hots.h"
#include "util/rte_memcpy.h"

#include <papi.h>

#define MEASURE_LATENCY 1

/* Shared structures between master and slave coroutines */
__thread int wrkr_gid;	/* Global ID of this worker */
__thread int wrkr_lid;	/* Local ID of this worker */
__thread int num_coro, base_port_index, num_ports, num_qps, postlist, numa_node;
__thread int num_machines, workers_per_machine, num_workers;

__thread size_t req_total = 0;
__thread uint64_t seed = 0xdeadbeef;

/* Application parameters */
__thread global_stats_t *global_stats;
__thread size_t size_req, size_resp;
__thread size_t req_batch_size;

__thread struct timespec msr_start, msr_end;	/* Throughput measurement */
__thread double lat_sum_us;	/* For latency measurement */

/* High-level HoTS structures */
__thread coro_call_t *coro_arr;
__thread coro_id_t *next_coro;
__thread Rpc *rpc;

/* A dummy request handler */
size_t rpc_test_rpc_handler(
	uint8_t *resp_buf, rpc_resptype_t *resp_type,
	const uint8_t *req_buf, size_t req_len, void *arg)
{
	rpc_dassert(resp_buf != NULL && req_buf != NULL && req_len == size_req);
	_unused(arg);
	
#if CHECK_RESP == 1
	/* Copy a byte from the request to the response */
	resp_buf[0] = req_buf[0];
#endif

	return size_resp;
}

void master_func(coro_yield_t &yield, int coro_id)
{
	assert(rpc != NULL);
	assert(coro_id == RPC_MASTER_CORO_ID);

	// Allow each slave to run once

	/* Create a temporary next_coro structure for initialization */
	next_coro = new coro_id_t[num_coro];
	for(int coro_i = 0; coro_i < num_coro; coro_i++) {
		next_coro[coro_i] = (coro_i == num_coro - 1) ? 0 : coro_i + 1;
	}

	yield(coro_arr[1]);
	delete next_coro;

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

	long long iter_coro = 0;	/* Request batches sent by this coroutine.
								 * Used for constructing request magic. */

	uint8_t magic __attribute__((unused)) = wrkr_gid + coro_id;
	
	rpc_req_t *req_arr[MAX_REQ_BATCH_SIZE] __attribute__((unused));
	uint8_t *resp_buf[MAX_REQ_BATCH_SIZE];
	for(size_t i = 0; i < req_batch_size; i++) {
		resp_buf[i] = (uint8_t *) memalign(8, size_resp);
	}

	/*
	 * Choose a random target machine to begin with. fastrand() is already
	 * shifted per thread; different coroutines use the same seed.
	 */
	int mn = hrd_fastrand(&seed) % num_machines;
	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {
		rpc->clear_req_batch(coro_id);

		for(size_t i = 0; i < req_batch_size; i++) {
			/* Choose a worker to send a message to */
#if USE_UNIQUE_WORKERS == 0
			mn = hrd_fastrand(&seed) % num_machines;
#else
			HRD_MOD_ADD(mn, num_machines);
#endif

			rpc_req_t *req = rpc->start_new_req(coro_id,
				RPC_MIN_REQ_TYPE, mn, resp_buf[i], size_req);
			req_arr[i] = req;	/* Save the request pointer for later */

			uint8_t req_magic __attribute__((unused)) = magic + iter_coro + i;
			req->req_buf[0] = req_magic;

			req->freeze(size_req);
		}

#if MEASURE_LATENCY == 1
		/* Start latency measurement */
		struct timespec lat_start, lat_end;	/* Need per-coroutine variables */
		clock_gettime(CLOCK_REALTIME, &lat_start);

		rpc->send_reqs(coro_id);
		yield(coro_arr[next_coro[coro_id]]);

		clock_gettime(CLOCK_REALTIME, &lat_end);

		lat_sum_us += (lat_end.tv_sec - lat_start.tv_sec) * 1000000 + 
				(double) (lat_end.tv_nsec - lat_start.tv_nsec) / 1000;
		
#else
		rpc->send_reqs(coro_id);
		yield(coro_arr[next_coro[coro_id]]);
#endif

#if CHECK_RESP == 1
		/* Check the response */
		for(size_t i = 0; i < req_batch_size; i++) {
			assert(req_arr[i]->resp_len == size_resp);
			uint8_t req_magic = magic + iter_coro + i;
			assert(resp_buf[i][0] == req_magic);
		}
#endif

		iter_coro++;
		req_total += req_batch_size;
		
		/* Any coroutine can print the measurement results */
		if(req_total >= M_4) {
			clock_gettime(CLOCK_REALTIME, &msr_end);
			/* Each of the two calls below zeroes out the corresponding stat */
			long long num_reqs = rpc->get_stat_num_reqs();
			long long num_creqs = rpc->get_stat_num_creqs();

			double msr_usec = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 + 
				(double) (msr_end.tv_nsec - msr_start.tv_nsec) / 1000;

			/* Fill in this worker's global stats */
			auto &gs = global_stats[wrkr_lid];
			gs.req_rate = (double) num_reqs / msr_usec;
			gs.creq_rate = (double) num_creqs / msr_usec;

			printf("Worker %d: requests/s = {%.2f M/s, %.2f M/s coalesced}, "
				"average latency per batch = %.2f\n", wrkr_lid,
				gs.req_rate, gs.creq_rate,
				MEASURE_LATENCY == 0 ?
					-1.0 : lat_sum_us / (req_total / req_batch_size));

			if(wrkr_lid == 0) {
				double req_rate_tot = 0;
				double creq_rate_tot = 0;

				for(int wrkr_i = 0; wrkr_i < workers_per_machine; wrkr_i++) {
					req_rate_tot += global_stats[wrkr_i].req_rate;
					creq_rate_tot += global_stats[wrkr_i].creq_rate;
				}

				hrd_red_printf("Machine req rate = "
					"%.2f M/s, %.2f M/s coalesced.\n",
					req_rate_tot, creq_rate_tot);
				fflush(stdout);
			}

			clock_gettime(CLOCK_REALTIME, &msr_start);

			rpc->print_stats();
			req_total = 0;
			lat_sum_us = 0;
		}
	}
}

// Parse configuration. Requires wrkr_gid to be set.
void parse_config()
{
	auto test_config =
		::mica::util::Config::load_file("rpc-test.json").get("rpc_test");

	num_coro = (int) test_config.get("num_coro").get_int64();
	base_port_index = (int) test_config.get("base_port_index").get_int64();
	num_ports = (int) test_config.get("num_ports").get_int64();
	num_qps = (int) test_config.get("num_qps").get_int64();
	postlist = (int) test_config.get("postlist").get_int64();
	numa_node = (int) test_config.get("numa_node").get_int64();
	num_machines = test_config.get("num_machines").get_int64();
	workers_per_machine = test_config.get("workers_per_machine").get_int64();
	req_batch_size = (int) test_config.get("req_batch_size").get_int64();
	size_req = (int) test_config.get("size_req").get_int64();
	size_resp = (int) test_config.get("size_resp").get_int64();
	
	assert(num_coro >= 2 && num_coro <= RPC_MAX_CORO);
	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(num_ports >= 1 && num_ports <= 8);
	assert(num_qps >= 1 && num_qps <= 4);
	assert(postlist >= 1 && postlist <= 64);
	assert(numa_node >= 0 && numa_node <= 3);
	assert(num_machines >= 0 && num_machines <= 256);
	assert(workers_per_machine >= 1 && workers_per_machine <= 56);
	assert(req_batch_size >= 1 && req_batch_size <= RPC_MAX_POSTLIST);
	assert(size_req > 0 && size_req <= 256 && size_req % sizeof(uint64_t) == 0);
	assert(size_resp > 0 && size_resp < RPC_MAX_MAX_PKT_SIZE &&
		size_resp % sizeof(uint32_t) == 0);

	// Derived parameters
	num_workers = num_machines * workers_per_machine;
	wrkr_lid = wrkr_gid % workers_per_machine;
}


void run_thread(struct thread_params *params)
{
	/* Intialize TLS variables */
	wrkr_gid = params->wrkr_gid;
	global_stats = params->global_stats;
	parse_config();

	size_t max_size = (size_req > size_resp) ? size_req : size_resp;

	/* Check if it is safe to use unique workers */
	if(USE_UNIQUE_WORKERS == 1) {
		assert(num_machines >= (int) req_batch_size);	/* Else no unique batch */
	}

	/* Move fastrand for this thread */
	for(int i = 0; i < wrkr_gid * 1000000; i++) {
		hrd_fastrand(&seed);
	}

	/* Initialize RPC. The RPC constructor will sanity-check args. */
	struct rpc_args _rpc_args = rpc_args(wrkr_gid, wrkr_lid,
		num_workers, workers_per_machine, num_coro,
		base_port_index, num_ports,
		num_qps, numa_node, postlist,
		(max_size + sizeof(rpc_cmsg_reqhdr_t)) * req_batch_size);

	cout << "Starting thread " << wrkr_gid << endl;
	rpc = new Rpc(_rpc_args);
	rpc->register_rpc_handler(RPC_MIN_REQ_TYPE, rpc_test_rpc_handler, NULL);

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
