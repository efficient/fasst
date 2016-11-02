#include "libhrd/hrd.h"
#include <thread>

#define SHM_KEY 24	/* For the lock array */
#define NUM_LOCKS (16384 * 128)
#define NUM_LOCKS_ (NUM_LOCKS - 1)

#define BATCH_SIZE 40	/* Number of outstanding transactions per lock thread */
#define NUM_ITEMS 16	/* Number of items per transaction */

struct my_lock {
	pthread_spinlock_t lock;
	int version;
	//long long pad[6];
};

struct thread_args {
	int tid;
	int num_threads;
	struct my_lock *lock_arr;
};

inline uint32_t fastrand(uint64_t* seed)
{
    *seed = *seed * 1103515245 + 12345;
    return (uint32_t) (*seed >> 32);
}

double *tput;

void *thread_function(struct thread_args *args)
{
	int i, j;
	int lock_num[BATCH_SIZE][NUM_ITEMS];
	bool batch_failed[BATCH_SIZE];

	int tid = args->tid;
	struct my_lock *lock_arr = args->lock_arr;

	/* Create a starting seed for this thread */
	uint64_t seed = 0xdeadbeef;
	for(i = 0; i < tid * 10000000; i++) {
		fastrand(&seed);
	}
	
	printf("Starting thread %d\n", tid);
	int iter = 0;
	int successful = 0;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	while(1) {
		if(iter >= 5000000) {
			clock_gettime(CLOCK_REALTIME, &end);
			double usec = (end.tv_sec - start.tv_sec) * 1000000 + 
				((double) (end.tv_nsec - start.tv_nsec) / 1000);
			printf("Thread %d, Tx attempts/s = %.2f M/s, "
				"Tx successful/s = %.2f M/s\n",
				tid, iter / usec, successful / usec);

			tput[tid] = successful / usec;

			if(tid == 0) {
				double total_mops = 0;
				for(i = 0; i < args->num_threads; i++) {
					total_mops += tput[i];
				}
				hrd_red_printf("Total tput = %.2f Mops\n", total_mops);
			}

			clock_gettime(CLOCK_REALTIME, &start);
			iter = 0;
			successful = 0;
		}

		for(i = 0; i < BATCH_SIZE; i++) {
			batch_failed[i] = false;
			for(j = 0; j < NUM_ITEMS; j++) {
				lock_num[i][j] = fastrand(&seed) & NUM_LOCKS_;
				//__builtin_prefetch(&lock_arr[lock_num[i][j]], 0, 0);
			}
		}

		for(i = 0; i < BATCH_SIZE; i++) {
			for(j = 0; j < NUM_ITEMS; j++) {
				int index = lock_num[i][j];
				int ret = pthread_spin_trylock(&lock_arr[index].lock);
				if(ret != 0) {
					batch_failed[i] = true;
					for(int k = 0; k < j; k++) {
						index = lock_num[i][k];
						pthread_spin_unlock(&lock_arr[index].lock);
					}

					break;
				}
			}
		}

		for(i = 0; i < BATCH_SIZE; i++) {
			if(batch_failed[i]) {
				continue;
			}

			successful++;

			for(j = 0; j < NUM_ITEMS; j++) {
				int index = lock_num[i][j];
				pthread_spin_unlock(&lock_arr[index].lock);
			}
		}

		iter += BATCH_SIZE;
	}
}

int main(int argc, char **argv)
{
	int i;

	assert(argc == 2);
	int num_threads = atoi(argv[1]);
	assert(num_threads >= 1);
	printf("Running %d threads\n", num_threads);

	printf("Lock array size  = %lu MB\n",
		(NUM_LOCKS * sizeof(struct my_lock)) / (1024 * 1024));
	struct my_lock *lock_arr = (struct my_lock *) hrd_malloc_socket(SHM_KEY,
		NUM_LOCKS * sizeof(struct my_lock), 0);
	assert(lock_arr != NULL);
	
	for(i = 0; i < NUM_LOCKS; i++) {
		lock_arr[i].version = 0;
		pthread_spin_init(&lock_arr[i].lock, 0);
	}
	
	struct thread_args *args_arr = new thread_args[num_threads];
	auto *thread_arr = new std::thread[num_threads];
	tput = new double[num_threads];
	for(i = 0; i < num_threads; i++) {
		tput[i] = 0;
	}
	
	for(i = 0; i < num_threads; i++) {
		args_arr[i].tid = i;
		args_arr[i].num_threads = num_threads;
		args_arr[i].lock_arr = lock_arr;

		thread_arr[i] = std::thread(thread_function, &args_arr[i]);
	}

	for(i = 0; i < num_threads; i++) {
		thread_arr[i].join();
	}

	return 0;
}

