#include "hrd.h"

#define BUF_SIZE 4096

void *run_server(void *arg)
{
	int s_gid = *(int *) arg;	/* Global ID of this server thread */
	int ib_port_index = 0;

	struct hrd_ctrl_blk *cb = hrd_ctrl_blk_init(s_gid,	/* local_hid */
		ib_port_index, -1, /* port_index, numa_node_id */
		1, 1, BUF_SIZE,	/* num_conn_qps, use_uc, conn_buf_size */
		0, 0);	/* num_dgram_qps, dgram_buf_size */

	hrd_publish_conn_qp(cb, 0, "server");
	printf("main: Server %d is now READY. Waiting for client..\n", s_gid);

	struct hrd_qp_attr *client_qp = NULL;
	while(client_qp == NULL) {
		client_qp = hrd_get_published_qp("client");
		if(client_qp == NULL) {
			usleep(200000);
		}
	}

	printf("Got client: name = %s. Connecting..\n", client_qp->name);
	hrd_connect_qp(cb->conn_qp[0], cb->port_id, client_qp);

	/* This garbles the server's qp_attr - which is safe */
	hrd_publish_ready("server");

	int i;
	for(i = 0; i < 100; i++) {
		printf("%d\n", cb->conn_buf[0]);
		sleep(1);
	}

	return NULL;
}

void *run_client(void *arg)
{
	int c_gid = *(int *) arg;	/* Global ID of this client thread */
	int ib_port_index = 0;

	struct hrd_ctrl_blk *cb = hrd_ctrl_blk_init(c_gid,	/* local_hid */
		ib_port_index, -1, /* port_index, numa_node_id */
		1, 1, BUF_SIZE,	/* num_conn_qps, use_uc, conn_buf_size */
		0, 0);	/* num_dgram_qps, dgram_buf_size */

	hrd_publish_conn_qp(cb, 0, "client");
	printf("main: Client %d is now READY. Waiting for server..\n", c_gid);

	struct hrd_qp_attr *server_qp = NULL;
	while(server_qp == NULL) {
		server_qp = hrd_get_published_qp("server");
		if(server_qp == NULL) {
			usleep(200000);
		}
	}

	printf("Got server: name = %s. Connecing..\n", server_qp->name);
	hrd_connect_qp(cb->conn_qp[0], cb->port_id, server_qp);

	hrd_wait_till_ready("server");

	struct ibv_send_wr wr, *bad_send_wr;
	struct ibv_sge sgl;
	int ret;

	wr.opcode = IBV_WR_RDMA_WRITE;
	wr.num_sge = 1;
	wr.next = NULL;
	wr.sg_list = &sgl;
	wr.send_flags = IBV_SEND_INLINE | IBV_SEND_SIGNALED;

	cb->conn_buf[0] = 23;
	sgl.addr = (uint64_t) (uintptr_t) (cb->conn_buf);
	sgl.length = 1;

	wr.wr.rdma.remote_addr = server_qp->buf_addr;
	wr.wr.rdma.rkey = server_qp->rkey;

	ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
	CPE(ret, "ibv_post_send error", ret);

	hrd_poll_cq(cb->conn_cq[0], 1);

	printf("Client exiting!\n");

	return NULL;
}

int main(int argc, char *argv[])
{
	int i, c;
	int num_threads = -1;
	int is_client = -1, machine_num = -1;	/* Client specific args */
	pthread_t *thread_arr;
	int *tid_arr;

	/* Parse and check arguments */
	while ((c = getopt(argc, argv, "t:c:m:")) != -1) {
		switch (c) {
			case 't':
				num_threads = atoi(optarg);
				break;
			case 'c':
				is_client = atoi(optarg);
				break;
			case 'm':
				machine_num = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				assert(false);
		}
	}

	assert(num_threads != -1);
	assert(is_client == 0 || is_client == 1);
	if(is_client == 1) {
		assert(machine_num >= 0);
	}

	printf("Using %d threads\n", num_threads);
	thread_arr = malloc(num_threads * sizeof(pthread_t));
	tid_arr = malloc(num_threads * sizeof(int));

	for(i = 0; i < num_threads; i++) {
		if(is_client) {
			tid_arr[i] = (machine_num * num_threads) + i;
			pthread_create(&thread_arr[i], NULL, run_client, &tid_arr[i]);
		} else {
			tid_arr[i] = i;
			pthread_create(&thread_arr[i], NULL, run_server, &tid_arr[i]);
		}
	}

	for(i = 0; i < num_threads; i++) {
		pthread_join(thread_arr[i], NULL);
	}

	return 0;
}
