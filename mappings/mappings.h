#ifndef MAPPINGS_H
#define MAPPINGS_H

#include <stdio.h>
#include <vector>
#include <set>
#include <climits>

#include "hots.h"
#include "rpc/rpc.h"	/* For rpc_dassert only */

class Mappings {
public:
	// Constructor args and derived structures
	int wrkr_gid;	/* ID of the this worker; required for should_i_populate() */
	int num_machines, workers_per_machine;
	int num_backups;
	bool use_lock_server;

	// Derived
	int machine_id;	/* Machine ID of this worker */

	int num_replicas;	/* 1 + num_backups */
	int tot_primary_workers; /* Total non-lockserver workers in the swarm */
	int tot_primary_machines;	/* Total non-lockserver machines in the swarm */
	int base_lockserver_wn;
	bool am_i_lock_server;

	/* Constructor */
	Mappings(int wrkr_gid,
		int num_machines, int workers_per_machine,
		int num_backups, bool use_lock_server):
		wrkr_gid(wrkr_gid),
		num_machines(num_machines), workers_per_machine(workers_per_machine),
		num_backups(num_backups), use_lock_server(use_lock_server) {

		assert(num_machines * workers_per_machine <= HOTS_MAX_WORKERS);
		assert(workers_per_machine <= HOTS_MAX_SERVER_THREADS);
		assert(num_backups >= 0 && num_backups <= HOTS_MAX_BACKUPS);
		assert(wrkr_gid >= 0 && wrkr_gid < num_machines * workers_per_machine);

		machine_id = wrkr_gid / workers_per_machine;
		num_replicas = 1 + num_backups;
		assert(num_replicas <= num_machines);

		/*
		 * Workers on all the @num_machines machines handle RPCs. If we use a
		 * lock server, the workers on the last machine are not Tx-level workers,
		 * i.e, they do not host data partitions.
		 */
		if(use_lock_server) {
			tot_primary_machines = num_machines - 1;

			/* The lock server workers are at the last machine */
			base_lockserver_wn = workers_per_machine * (num_machines - 1);
		} else {
			tot_primary_machines = num_machines;
			base_lockserver_wn = INT_MIN;
		}

		tot_primary_workers = workers_per_machine * tot_primary_machines;

		am_i_lock_server = use_lock_server ?
			(machine_id == num_machines - 1) : false;
	}

	forceinline int get_primary_mn(uint64_t keyhash)
	{
		/*
		 * Lower-order keyhash bits are used to map to buckets. @keyhash has
		 * at least 48 useful bits, so shifting by 32 is OK.
		 */
		return ((keyhash >> 32) % tot_primary_machines);
	}

	/* Get the backup machine with index @back_i (0-based) for this primary */
	forceinline int get_backup_mn_from_primary(int primary_mn, int back_i)
	{
		rpc_dassert(primary_mn >= 0 && primary_mn < tot_primary_machines);
		rpc_dassert(back_i >= 0 && back_i < num_backups);

		/* Backup machines are the subsequent machines in the ring */
		int backup_mn = primary_mn + (back_i + 1);
		if(backup_mn >= tot_primary_machines) {
			backup_mn -= tot_primary_machines;
		}

		rpc_dassert(backup_mn >= 0 && backup_mn < tot_primary_machines);
		return backup_mn;
	}

	forceinline int get_backup_mn(uint64_t keyhash, int back_i)
	{
		return get_backup_mn_from_primary(get_primary_mn(keyhash), back_i);
	}

	/*
	 * Should this worker insert a key during initial table population?
	 * @repl_i is a 0-based replica ID. @repl_i is 0 for the primary, and 1 or
	 * 2 for the backup replicas.
	 */
	forceinline bool should_i_populate(uint64_t keyhash, int repl_i)
	{
		rpc_dassert(repl_i >= 0 && repl_i < HOTS_MAX_REPLICAS);

		int _owner_wn;	/* This worker populates @keyhash */

		/*
		 * During transactions, requests for @keyhash will be sent to any thread
		 * on @primary_machine or @backup_machine computed below. We choose a
		 * deterministic owner on that machine to insert the key during initial
		 * population. The thread must be chosen using a different keyhash slice
		 * (>> 20).
		 */
		if(repl_i == 0) {
			/* Populating as primary */
			int primary_machine = get_primary_mn(keyhash);
			_owner_wn = (primary_machine * workers_per_machine) +
				(keyhash >> 20) % workers_per_machine;
		} else {
			/* Populating as backup (@repl_i - 1) */
			int backup_machine = get_backup_mn(keyhash, repl_i - 1);
			_owner_wn = (backup_machine * workers_per_machine) +
				(keyhash >> 20) % workers_per_machine;
		}

		return wrkr_gid == _owner_wn;
	}

	/* Get a log replica from this worker's global ID */
	forceinline int get_log_mn(int back_i)
	{
		rpc_dassert(back_i >= 0 && back_i < num_backups);
		return get_backup_mn_from_primary(machine_id, back_i);
	}

	forceinline int get_lockserver_mn()
	{
		rpc_dassert(use_lock_server);

		/*
		 * When lockserver is used and there are N total machines,
		 * @tot_primary_machines is N-1, which is equal to the machine ID of
		 * the lockserver machine
		 */
		return tot_primary_machines;
	}
};

#endif /* MAPPINGS_H */
