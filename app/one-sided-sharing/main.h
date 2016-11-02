#define SERVER_SHM_KEY 1
#define NUM_CLIENT_MACHINES 5
#define MAX_WINDOW 64

#define BUF_SIZE M_8
#define BUF_SIZE_ (M_8 - 1)

/*
 * If this is 1, server threads use exclusive QPs, i.e., server thread i chooses
 * QPs that are i modulo the total number of QPs.
 */
#define USE_EXCLUSIVE_QPS 0

struct qp_info_t {
	/* Collected QPs and CQs, and RDMA addresses from different ports */
	struct ibv_qp *qp;
	struct ibv_cq *cq;

	uint64_t local_addr;
	uint32_t lkey;

	uint64_t remote_addr;
	uint32_t rkey;
	
	uint8_t pad[64];	/* This struct is read-only, but pad it anyway */
};

struct thread_params {
	int num_threads;
	int tid;
	int num_qps;
	int window_size;
	int size;
	int do_read;

	qp_info_t *qp_info;
};
