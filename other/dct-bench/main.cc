#include <infiniband/verbs_exp.h>
#include <getopt.h>
#include <thread>
#include "libhrd/hrd.h"
#include "main.h"

/* Create a DCT target for this machine and publish it */
void create_and_publish_dct_target(int machine_id, int num_ports)
{
	struct ibv_device **dev_list = ibv_get_device_list(NULL);
	assert(dev_list != NULL);

	struct ibv_device *ib_dev = dev_list[0]; /* Use the first device */
	assert(ib_dev != NULL);

	struct ibv_context *ctx = ibv_open_device(ib_dev);
	assert(ctx != NULL);
	check_dct_supported(ctx);

	/* Create protection domain */
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	assert(pd != NULL);

	/* Register an RDMA buffer */
	uint8_t *rdma_buf = (uint8_t *) malloc(DCTBENCH_BUF_SIZE);
	assert(rdma_buf != NULL);

	struct ibv_mr *mr = ibv_reg_mr(pd, rdma_buf, DCTBENCH_BUF_SIZE,
		IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_READ);
	assert(mr != NULL);

	/* Create CQ. We don't use it at the target. */
	struct ibv_cq *cq = ibv_create_cq(ctx, 128, NULL, NULL, 0);
	assert(cq != NULL);

	/* Create SRQ. SRQ is required to init DCT target, but we don't used it */
	struct ibv_srq_init_attr attr;
	memset((void *) &attr, 0, sizeof(attr));
	attr.attr.max_wr = 100;
	attr.attr.max_sge = 1;
	struct ibv_srq *srq = ibv_create_srq(pd, &attr);
	assert(srq != NULL);

	/* Create one DCT target per port */
	for(int port_i = 0; port_i < num_ports; port_i++) {
		struct ibv_exp_dct_init_attr dctattr;
		memset((void *) &dctattr, 0, sizeof(dctattr));
		dctattr.pd = pd;
		dctattr.cq = cq;
		dctattr.srq = srq;
		dctattr.dc_key = DCTBENCH_DCT_KEY;
		dctattr.port = port_i + 1;
		dctattr.access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
		dctattr.min_rnr_timer = 2;
		dctattr.tclass = 0;
		dctattr.flow_label = 0;
		dctattr.mtu = IBV_MTU_4096;
		dctattr.pkey_index = 0;
		dctattr.hop_limit = 1;
		dctattr.create_flags = 0;
		dctattr.inline_size = HRD_MAX_INLINE;

		struct ibv_exp_dct *dct = ibv_exp_create_dct(ctx, &dctattr);
		assert(dct != NULL);

		check_dct_healthy(dct);
		
		/* Publish DCT info */
		struct dct_attr_t dct_attr;
		dct_attr.lid = hrd_get_local_lid(ctx, port_i + 1);
		dct_attr.dct_num = dct->dct_num;
		dct_attr.buf_addr = (uintptr_t) rdma_buf;
		dct_attr.rkey = mr->rkey;

		printf("Server sending info: lid = %u, dct_num = %u\n",
			(unsigned) dct_attr.lid, (unsigned) dct_attr.dct_num);

		char dct_target_name[HRD_QP_NAME_SIZE];
		sprintf(dct_target_name, "target-%d-%d", machine_id, port_i);
		hrd_publish(dct_target_name, &dct_attr, sizeof(dct_attr_t));
	}
}

int main(int argc, char *argv[])
{
	static_assert(HRD_MAX_INLINE == 60, "");

	auto test_config =
		::mica::util::Config::load_file("dct-test.json").get("dct_test");

	int num_ports = test_config.get("num_ports").get_int64();
	int workers_per_machine = test_config.get("workers_per_machine").get_int64();
	int num_machines = test_config.get("num_machines").get_int64();
	_unused(num_machines);

	int machine_id = -1;
	static struct option opts[] = {
		{"machine-id", required_argument, 0, 'm' },
		{0, 0, 0, 0}
	};
		
	/* Parse and check arguments */
	int c;
	while(1) {
		c = getopt_long(argc, argv, "m", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 'm':
				machine_id = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				exit(-1);
		}
	}

	/* Sanity checks */
	assert(machine_id >= 0 && machine_id < num_machines);

	printf("main: Publishing DCT targets for machine %d (%d ports)\n",
		machine_id, num_ports);
	create_and_publish_dct_target(machine_id, num_ports);

	printf("main: Launching %d swarm workers\n", workers_per_machine);
	double *tput = new double[workers_per_machine];
	long long *reqs_ever = new long long[workers_per_machine];
	for(int i = 0; i < workers_per_machine; i++) {
		tput[i] = 0.0;
		reqs_ever[i] = 0;
	}

	auto param_arr = new struct thread_params[workers_per_machine];
	auto thread_arr = new std::thread[workers_per_machine];
	auto global_stats = new global_stats_t[workers_per_machine];

	for(int i = 0; i < workers_per_machine; i++) {
		param_arr[i].wrkr_gid = (machine_id * workers_per_machine) + i;
		param_arr[i].global_stats = global_stats;
		
		thread_arr[i] = std::thread(run_thread, &param_arr[i]);

		/* Pin thread i to hardware thread 2 * i */
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(2 * i, &cpuset);
		int rc = pthread_setaffinity_np(thread_arr[i].native_handle(),
			sizeof(cpu_set_t), &cpuset);
		if (rc != 0) {
			printf("Error calling pthread_setaffinity_np\n");
		}
	}

	for(int i = 0; i < workers_per_machine; i++) {
		printf("main: waiting for thread %d\n", i);
		thread_arr[i].join();
		printf("main: thread %d done\n", i);
	}

	return 0;
}
