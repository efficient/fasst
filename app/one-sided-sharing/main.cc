#include "libhrd/hrd.h"
#include "hots.h"
#include "main.h"

#include <malloc.h>
#include <getopt.h>
#include <thread>

//#define get_cycles() get_cycles()
#define get_cycles() 0

void run_thread(struct thread_params *params)
{
	printf("Server: Starting thread %d\n", params->tid);
	/* Move seed for this thread */
	uint64_t seed = 0xdeadbeef;
	for(int i = 0; i < params->tid * 1000000; i++) {
		hrd_fastrand(&seed);
	}

	struct ibv_send_wr wr, *bad_send_wr;
	struct ibv_sge sgl;
	struct ibv_wc wc;
	long long rolling_iter = 0;	/* For performance measurement, gets reset */
	long long nb_tx_tot = 0;
	int window_i = 0;
	int which_qp[MAX_WINDOW] = {0};	/* Record which QP we used for a window slot */
	int ret;

	struct timespec start, end;
	long long post_send_cycles_tot = 0;
	long long poll_cq_cycles_tot = 0;
	long long loopiter_cycles_tot = 0;
	clock_gettime(CLOCK_REALTIME, &start);

	int opcode = params->do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

	while(1) {
		if(rolling_iter >= M_2) {
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) + 
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

			printf("main: Thread %d: %.2f Mops. "
				"Cycles/post_send = %lld, cycles/poll_cq = %lld, "
				"cycles/loop iter = %lld\n",
				params->tid, M_2 / seconds,
				post_send_cycles_tot / rolling_iter,
				poll_cq_cycles_tot / rolling_iter,
				loopiter_cycles_tot / rolling_iter);
			
			rolling_iter = 0;
			post_send_cycles_tot = 0;
			poll_cq_cycles_tot = 0;
			loopiter_cycles_tot = 0;

			clock_gettime(CLOCK_REALTIME, &start);
		}

		long long loopiter_cycles_start = get_cycles();

		if(nb_tx_tot >= params->window_size) {
			int cq_to_poll = which_qp[window_i];
			long long poll_cq_cycles_start = get_cycles();
			hrd_poll_cq(params->qp_info[cq_to_poll].cq, 1, &wc);
			poll_cq_cycles_tot += (get_cycles() - poll_cq_cycles_start);
		}

		/* Choose a QP */
		int _qp_i = hrd_fastrand(&seed) % params->num_qps;
#if USE_EXCLUSIVE_QPS
		while(_qp_i % params->num_threads != params->tid) {
			_qp_i = hrd_fastrand(&seed) % params->num_qps;
		}
#endif
		which_qp[window_i] = _qp_i;
		qp_info_t *_qp_info = &params->qp_info[_qp_i];

		wr.opcode = (ibv_wr_opcode) opcode;
		wr.num_sge = 1;
		wr.next = NULL;
		wr.sg_list = &sgl;

		wr.send_flags = IBV_SEND_SIGNALED;
		wr.send_flags |= (params->do_read == 0) ? IBV_SEND_INLINE: 0;

		/*
		 * Aligning local/remote offset to 64-byte boundary REDUCES performance
		 * significantly (similar to atomics).
		 */
		int _offset = (hrd_fastrand(&seed) & BUF_SIZE_);
		while(_offset >= BUF_SIZE - params->size) {
			_offset = (hrd_fastrand(&seed) & BUF_SIZE_);
		}

		/*
		 * We do not read the data that the NIC DMA's, so no need for exclusive
		 * per-thread DMA regions.
		 */
		sgl.addr = _qp_info->local_addr + _offset;

		sgl.length = params->size;
		sgl.lkey = _qp_info->lkey;

		wr.wr.rdma.remote_addr = _qp_info->remote_addr + _offset;
		wr.wr.rdma.rkey = _qp_info->rkey;

		long long post_send_cycles_start = get_cycles();
		ret = ibv_post_send(params->qp_info[_qp_i].qp, &wr, &bad_send_wr);
		CPE(ret, "ibv_post_send error", ret);
		post_send_cycles_tot += (get_cycles() - post_send_cycles_start);

		rolling_iter++;
		nb_tx_tot++;
		HRD_MOD_ADD(window_i, params->window_size);

		loopiter_cycles_tot += (get_cycles() - loopiter_cycles_start);
	}

	return;
}

int main(int argc, char *argv[])
{
	int num_threads = -1, num_qps = -1, base_port_index = -1, num_ports = -1;
	int use_uc = -1, numa_node = -1;
	int is_client = -1, machine_id = -1, size = -1, window_size = -1;
	int do_read = -1;

	/*
	 * num-threads: Number of threads sharing QPs at the server machines.
	 * Client machines run one thread each.
	 * 
	 * num-qps: Number of QPs shared by the threads at the server machine.
	 * Each client machine supplies (num_qps / NUM_CLIENT_MACHINES) QPs
	 */
	static struct option opts[] = {
		{"num-threads", required_argument, 0, 't' },
		{"num-qps", required_argument, 0, 'q' },
		{"base-port-index", required_argument, 0, 'b' },
		{"num-ports", required_argument, 0, 'N' },
		{"numa-node", required_argument, 0, 'n' },
		{"use-uc", required_argument, 0, 'u' },
		{"is-client", required_argument, 0, 'c' },
		{"machine-id", required_argument, 0, 'm' },
		{"size", required_argument, 0, 's' },
		{"window-size", required_argument, 0, 'w' },
		{"do-read", required_argument, 0, 'r' },
	};

	/* Parse and check arguments */
	while(1) {
		int c = getopt_long(argc, argv, "t:q:b:N:n:u:c:m:s:w:r", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 't':
				num_threads = atoi(optarg);
				break;
			case 'q':
				num_qps = atoi(optarg);
				break;
			case 'b':
				base_port_index = atoi(optarg);
				break;
			case 'N':
				num_ports = atoi(optarg);
				break;
			case 'n':
				numa_node = atoi(optarg);
				break;
			case 'u':
				use_uc = atoi(optarg);
				break;
			case 'c':
				is_client = atoi(optarg);
				break;
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 's':
				size = atoi(optarg);
				break;
			case 'w':
				window_size = atoi(optarg);
				break;
			case 'r':
				do_read = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				assert(false);
		}
	}

	assert(num_qps >= 1 && num_qps % NUM_CLIENT_MACHINES == 0);
	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(num_ports >= 1 && num_ports <= 8);
	assert(use_uc == 0 || use_uc == 1);
	assert(is_client == 0 || is_client == 1);

	if(is_client == 1) {
		assert(num_threads == -1);	/* Only 1 thread at clients */
		assert(machine_id >= 0);
		assert(size == -1 && window_size == -1 && do_read == -1);
	} else {
		assert(num_threads >= 1);
		assert(machine_id == -1);
		assert(size >= 0);

		if(do_read == 0) {	/* We always do inlined WRITEs */
			assert(size <= HRD_MAX_INLINE);
		}

		assert(window_size >= 1 && window_size <= MAX_WINDOW);
		assert(window_size <= HRD_SQ_DEPTH);

		assert(do_read == 0 || do_read == 1);
	}

	if(is_client == 0) {
		auto clt_qp_arr = new hrd_qp_attr*[num_qps];
		auto cb = new hrd_ctrl_blk*[num_qps];	/* Per-QP control blocks */

		// Server
		for(int qp_i = 0; qp_i < num_qps; qp_i++) {
			int port_i = qp_i % num_ports;
			int ib_port_index = base_port_index + port_i;

			volatile void *prealloc_conn_buf =
				(qp_i == 0 ? NULL : cb[0]->conn_buf);

			int shm_key = (qp_i == 0 ? SERVER_SHM_KEY : -1);

			cb[qp_i] = hrd_ctrl_blk_init(qp_i, /* Local hid */
				ib_port_index, numa_node, /* port index, numa node id */
				1, use_uc,	/* #conn_qps, use_uc */
				prealloc_conn_buf, BUF_SIZE, shm_key,
				NULL, 0, 0, -1);	/* #dgram qps, buf size, shm key */
		}

		/* Publish QPs. Each port has @num_qps QPs, but we only publish some. */
		for(int qp_i = 0; qp_i < num_qps; qp_i++) {
			char srv_name[HRD_QP_NAME_SIZE];
			sprintf(srv_name, "server-%d", qp_i);
			hrd_publish_conn_qp(cb[qp_i], 0, srv_name);
		}

		/* Connect to client QPs */
		for(int qp_i = 0; qp_i < num_qps; qp_i++) {
			char clt_name[HRD_QP_NAME_SIZE];
			sprintf(clt_name, "client-%d", qp_i);
			printf("main: Server waiting for client %s\n", clt_name);

			clt_qp_arr[qp_i] = NULL;
			while(clt_qp_arr[qp_i] == NULL) {
				clt_qp_arr[qp_i] = hrd_get_published_qp(clt_name);
				if(clt_qp_arr[qp_i] == NULL) {
					usleep(200000);
				}
			}
		
			hrd_connect_qp(cb[qp_i], 0, clt_qp_arr[qp_i]);
			hrd_wait_till_ready(clt_name);
		}

		auto param_arr = new thread_params[num_threads];
		auto thread_arr = new std::thread[num_threads];
	
		for(int i = 0; i < num_threads; i++) {
			param_arr[i].num_threads = num_threads;
			param_arr[i].tid = i;
			param_arr[i].num_qps = num_qps;
			param_arr[i].window_size = window_size;
			param_arr[i].size = size;
			param_arr[i].do_read = do_read;

			param_arr[i].qp_info = (qp_info_t *) memalign(4096,
				num_qps * sizeof(qp_info_t));

			for(int qp_i = 0; qp_i < num_qps; qp_i++) {
				qp_info_t *qp_info = &param_arr[i].qp_info[qp_i];
				qp_info->qp = cb[qp_i]->conn_qp[0];
				qp_info->cq = cb[qp_i]->conn_cq[0];

				qp_info->local_addr = (uint64_t) cb[qp_i]->conn_buf;
				qp_info->lkey = cb[qp_i]->conn_buf_mr->lkey;

				qp_info->remote_addr = clt_qp_arr[qp_i]->buf_addr;
				qp_info->rkey = clt_qp_arr[qp_i]->rkey;
			}

			thread_arr[i] = std::thread(run_thread, &param_arr[i]);

			/* Pin thread i to hardware thread 2 * i */
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(2 * i, &cpuset);
			int rc = pthread_setaffinity_np(thread_arr[i].native_handle(),
				sizeof(cpu_set_t), &cpuset);
			if(rc != 0) {
				printf("Error %d while calling pthread_setaffinity_np\n", rc);
			}
		}

		for(int i = 0; i < num_threads; i++) {
			printf("main: waiting for thread %d\n", i);
			thread_arr[i].join();
			printf("main: thread %d done\n", i);
		}
	} else {
		// Client
		int num_qps_clt = num_qps / NUM_CLIENT_MACHINES;
		auto cb = new hrd_ctrl_blk*[num_ports];	/* Per-port control blocks */

		for(int port_i = 0; port_i < num_ports; port_i++) {
			volatile void *prealloc_conn_buf =
				(port_i == 0 ? NULL : cb[0]->conn_buf);

			int ib_port_index = base_port_index + port_i;
			int shm_key = (port_i == 0 ? SERVER_SHM_KEY : -1);

			cb[port_i] = hrd_ctrl_blk_init(port_i, /* Local hid */
				ib_port_index, 0, /* port index, numa node id */
				num_qps_clt, use_uc,	/* #conn_qps, use_uc */
				prealloc_conn_buf, BUF_SIZE, shm_key,
				NULL, 0, 0, -1);	/* #dgram qps, buf size, shm key */
		}

		int qp_lo = num_qps_clt * machine_id;
		int qp_hi = num_qps_clt * (machine_id + 1);

		/* Publish QPs. Each port has @num_qps_clt QPs, but only publish some. */
		for(int qp_i = qp_lo; qp_i < qp_hi; qp_i++) {
			int port_i = qp_i % num_ports;
			int _qp_i = qp_i - qp_lo;

			char clt_name[HRD_QP_NAME_SIZE];
			sprintf(clt_name, "client-%d", qp_i);
			hrd_publish_conn_qp(cb[port_i], _qp_i, clt_name);
		}

		/* Connect to server */
		for(int qp_i = qp_lo; qp_i < qp_hi; qp_i++) {
			char srv_name[HRD_QP_NAME_SIZE];
			sprintf(srv_name, "server-%d", qp_i);
			printf("main: Client machine %d waiting for server %s\n",
				machine_id, srv_name);
			
			struct hrd_qp_attr *srv_qp = NULL;
			while(srv_qp == NULL) {
				srv_qp = hrd_get_published_qp(srv_name);
				if(srv_qp == NULL) {
					usleep(200000);
				}
			}
		
			int port_i = qp_i % num_ports;
			int _qp_i = qp_i - qp_lo;	/* Local QP index */
			hrd_connect_qp(cb[port_i], _qp_i, srv_qp);

			printf("\tmain: Connected!\n");

			char clt_name[HRD_QP_NAME_SIZE];
			sprintf(clt_name, "client-%d", qp_i);	/* Use global QP index */
			hrd_publish_ready(clt_name);
		}

		printf("main: Client machine %d going to sleep\n", machine_id);
		sleep(1000000);
	}

	assert(false);	/* Never come here */
	return 0;
}
