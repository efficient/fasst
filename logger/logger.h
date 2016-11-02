#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <pthread.h>

#include "hots.h"
#include "rpc/rpc.h"
#include "tx/tx_defs.h"
#include "libhrd/hrd.h"
#include "util/rte_memcpy.h"
#include "mica/util/barrier.h"

#define log_magic 17	/* Some 5-bit number */

struct log_record_t {
	uint32_t mchn_id :HOTS_MCHN_ID_BITS;	/* Machine ID of the coordinator */
	uint32_t coro_id :HOTS_CORO_ID_BITS; 	/* Coro ID of the coordinator */
	uint32_t magic :5;	/* Debug-only */
	uint32_t debug_size :13;	/* Total log record size; debug-only. */
	uint32_t num_keys;
	/* 64 bits up to here */

	uint8_t buf[RPC_MAX_MAX_PKT_SIZE];
};
static_assert(sizeof(log_record_t) ==
	RPC_MAX_MAX_PKT_SIZE + sizeof(uint64_t), "");

enum class logger_resptype_t : uint16_t {
	success = 3,
};

/*
 * Each worker thread creates one Logger object.
 *
 * In the context of this Logger, a coordinator coroutine is uniquely identified
 * by the combination of its machine ID, and its thread-local coroutine ID. The
 * worker ID of the coordinator is not required: it can be inferred from its
 * machine ID, and the worker ID of the thread that creates this Logger.
 *
 * During recovery (which we have not implemented), we need to know which log
 * records are valid. In FaSST, all log records with a non-zero @num_keys are
 * valid. This is because the RPC handler that saves the log record is a single
 * function, and cannot return to the master coroutine's polling loop before
 * saving the entire log record. The handler can be interrupted if this machine
 * fails, but then we've lost the log record.
 */
class Logger {
private:
	// Constructor args
	int wrkr_gid, wrkr_lid;	/* IDs of the worker that creates this Logger */
	int num_machines;	/* Total machines in the swarm */
	int num_coro;	/* Coroutines per thread */

	// Derived
	log_record_t *log;	/* One log record per coroutine in the cluster */

public:

	Logger(int wrkr_gid, int wrkr_lid, int num_machines, int num_coro) :
		wrkr_gid(wrkr_gid), wrkr_lid(wrkr_lid), num_machines(num_machines),
		num_coro(num_coro)
	{
		/*
		 * Initialize hugepage memory for log records. At each machine in the
		 * swarm, there are @num_coro coroutines that will send log requests to
		 * this Logger.
		 */
		size_t reqd_records = num_machines * num_coro;
		size_t reqd_size = reqd_records * sizeof(log_record_t);
		while(reqd_size % M_2 != 0) {
			reqd_size++;
		}

		int shm_key = LOGGER_BASE_SHM_KEY + wrkr_lid;
		uint8_t *buf = (uint8_t *) hrd_malloc_socket(shm_key,
			reqd_size, 0);	/* Returns zeroed-out memory */
		assert(buf != NULL);

		log = (log_record_t *) buf;
	}

	/* Get a pointer to the log record for this coroutine */
	forceinline log_record_t* get_log_record(int mchn_id, int coro_id)
	{
		tx_dassert(mchn_id >= 0 && mchn_id < num_machines);
		tx_dassert(coro_id >= 1 && coro_id < num_coro);

		int record_idx = (mchn_id * num_coro) + coro_id;
		return &log[record_idx];
	}

	forceinline void save_log_record(log_record_t *log_record,
		size_t record_size)
	{
		tx_dassert(log_record != NULL);
		tx_dassert(record_size <= sizeof(struct log_record_t));

		tx_dassert(log_record->mchn_id >= 0 &&
			log_record->mchn_id < num_machines);
		tx_dassert(log_record->coro_id >= 1 &&
			log_record->coro_id < num_coro);

		int record_idx = (log_record->mchn_id * num_coro) +
			log_record->coro_id;

		rte_memcpy((void *) &log[record_idx], (void *) log_record, record_size);
	}
};

forceinline size_t logger_rpc_handler(
	uint8_t *resp_buf, rpc_resptype_t *resp_type,
	const uint8_t *req_buf, size_t req_len, void *_logger)
{
	log_record_t *log_record = (log_record_t *) req_buf;
	tx_dassert(log_record->magic == log_magic);
	tx_dassert(log_record->debug_size == req_len);

	Logger *logger = static_cast<Logger *>(_logger);
	logger->save_log_record(log_record, req_len);

	*resp_type = (uint16_t) logger_resptype_t::success;
	return 0;
}

#endif
