#include <stdio.h>
#include <math.h>

int main()
{
	double lockserver_tput = 70.0;
	double machine_req_rate = 40.0;	/* Request rate of each machine in swarm */
	
	int RS, WS;	/* Read set includes write set */
	for(RS = 1; RS <= 7; RS++) {
		for(WS = 0; WS <= RS; WS++) {
			int execute_messages = RS;
			int validate_messages = RS - WS;
			int log_messages = (WS == 0) ? 0 : 2;	/* 3-way replication */
			/*
			 * With 3-way replication, a naive scheme sends 3 * WS messages to
			 * update remote records. With a FaRM-like scheme, we can batch
			 * updates to backups. A wild guess could be a reduction from
			 * 3 * WS to 2 * WS.
			 */
			int update_messages = (WS * 2);
			int tot_messages = (execute_messages + validate_messages +
				log_messages + update_messages);

			/* Transaction throughput of each machine */
			double machine_tx_tput = machine_req_rate / tot_messages;
			printf("RS = %d, WS = %d, machine transaction throughput = %.2f, "
				"messages per transaction = %d, "
				"supported machines = %.0f\n", RS, WS, machine_tx_tput,
				tot_messages,
				ceil(lockserver_tput / machine_tx_tput));
		}
	}
}
