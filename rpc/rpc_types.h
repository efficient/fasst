#include<string>

#ifndef RPC_TYPES_H
#define RPC_TYPES_H

#define RPC_MIN_REQ_TYPE 0	/* Minimum type for requests */
#define RPC_MAX_REQ_TYPE 255	/* Maximum type for requests */

#define RPC_IS_VALID_TYPE(x) (x >= RPC_MIN_REQ_TYPE && x <= RPC_MAX_REQ_TYPE)

// Subsystems
#define RPC_LOCKSERVER_REQ 1	/* Lock server */
#define RPC_LOGGER_REQ 2		/* Logger */


// Datastores. If the RPC type for a store is n, then types n + 1, and n + 2
// are used for its backup stores. So the gap between the base defined types
// for different tables should be at least 3; we use a gap of 10 for simplicity.

#define RPC_PRIMARY_DS_REQ_SPACING 10


// MICA datastores. All RPC types larger than RPC_MICA_REQ_BASE must be MICA
// requests. This is required for prefetching in the RPC datapath.

#define RPC_MICA_REQ_BASE 20

/* dist-kv */
#define RPC_MICA_REQ 20

/* tatp */
#define RPC_SUBSCRIBER_REQ 20
#define RPC_SEC_SUBSCRIBER_REQ 30
#define RPC_SPECIAL_FACILITY_REQ 40
#define RPC_ACCESS_INFO_REQ 50
#define RPC_CALL_FORWARDING_REQ 60

/* smallbank */
#define RPC_SAVING_REQ 70
#define RPC_CHECKING_REQ 80

/* stress */
#define RPC_STRESS_TABLE_REQ 90

static std::string rpc_type_to_string(int rpc_type)
{
	switch(rpc_type) {
		case RPC_MIN_REQ_TYPE:
			return std::string("RPC_MIN_REQ_TYPE");
		case RPC_LOCKSERVER_REQ:
			return std::string("RPC_LOCKSERVER_REQ");
		case RPC_LOGGER_REQ:
			return std::string("RPC_LOGGER_REQ");
		case RPC_MICA_REQ:
			return std::string("RPC_MICA_REQ-primary");
		case RPC_MICA_REQ + 1:
			return std::string("RPC_MICA_REQ-backup-1");
		case RPC_MICA_REQ + 2:
			return std::string("RPC_MICA_REQ-backup-2");
		default:
			return std::string("Invalid or unhandled");
	};
}

#endif /* RPC_TYPES */
