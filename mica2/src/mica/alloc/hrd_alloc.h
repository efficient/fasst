#ifndef HRD_ALLOC_H
#define HRD_ALLOC_H

#include <errno.h>
#include <malloc.h>
#include <numaif.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include "mica/util/roundup.h"
#include "mica/util/config.h"

namespace mica {
namespace alloc {

/* Code taken from libhrd */
class HrdAlloc {
public:
	HrdAlloc(const ::mica::util::Config& config)
	{
	}

	static size_t roundup(size_t size)
	{
		return ::mica::util::roundup<2 * 1048576>(size);
	}

	void* hrd_malloc_socket(int shm_key, size_t size, int numa_node)
	{
		size = roundup(size);
		int shmid = shmget(shm_key,
			size, IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);
		if(shmid == -1) {
			switch(errno) {
				case EACCES:
					printf("HrdAlloc: SHM malloc error: "
						"Insufficient permissions. (SHM key = %d)\n", shm_key);
					break;
				case EEXIST:
					printf("HrdAlloc: SHM malloc error: Already exists."
						" (SHM key = %d)\n", shm_key);
					break;
				case EINVAL:
					printf("HrdAlloc: SHM malloc error: SHMMAX/SHMIN "
						"mismatch. (SHM key = %d, size = %lu)\n", shm_key, size);
					break;
				case ENOMEM:
					printf("HrdAlloc: SHM malloc error: Insufficient memory."
						" (SHM key = %d, size = %lu)\n", shm_key, size);
					break;
				default:
					printf("HrdAlloc: SHM malloc error: Wild SHM error: %s.\n",
						strerror(errno));
					break;
			}
			assert(false);
		}

		void *buf = shmat(shmid, NULL, 0);
		if(buf == NULL) {
			printf("HrdAlloc: SHM malloc error: shmat() failed for key %d\n",
				shm_key);
			exit(-1);
		}

		/* Bind the buffer to this socket */
		const unsigned long nodemask = (1 << numa_node);
		int ret = mbind(buf, size, MPOL_BIND, &nodemask, 32, 0);
		if(ret != 0) {
			printf("HrdAlloc: SHM malloc error. mbind() failed for key %d\n",
				shm_key);
			exit(-1);
		}

		memset(buf, 0, size);
		return buf;
	}

	bool hrd_free(int shm_key, void *shm_buf)
	{
		int ret;
		int shmid = shmget(shm_key, 0, 0);
		if(shmid == -1) {
			switch(errno) {
				case EACCES:
					printf("HrdAlloc: SHM free error: "
						"Insufficient permissions.  (SHM key = %d)\n", shm_key);
					break;
				case ENOENT:
					printf("HrdAlloc: SHM free error: No such SHM key."
						" (SHM key = %d)\n", shm_key);
					break;
				default:
					printf("HrdAlloc: SHM free error: A wild SHM error: %s\n",
						strerror(errno));
					break;
			}
			return -1;
		}

		ret = shmctl(shmid, IPC_RMID, NULL);	/* Please don't fail */
		if(ret != 0) {
			printf("HrdAlloc: Error freeing SHM ID %d\n", shmid);
			return false;
		}

		ret = shmdt(shm_buf);
		if(ret != 0) {
			printf("HrdAlloc: Error freeing SHM buf %p (SHM key = %d)\n",
				shm_buf, shm_key);
			return false;
		}

		return true;
	}
};
}
}

#endif
