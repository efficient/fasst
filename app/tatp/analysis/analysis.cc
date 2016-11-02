#include <stdio.h>
#include <vector>
#include <assert.h>
#include <stdlib.h>

#define ITERS 10000000

struct cf_record_t {
	int start_time;
	int end_time;

	cf_record_t(int start_time, int end_time) :
		start_time(start_time), end_time(end_time) {
		assert(start_time == 0 || start_time == 8 || start_time == 16);
		assert(end_time >= 1 && end_time <= 24);
	}
};

/*
 * Computes a probability value required to determine the success rate of the
 * GET_NEW_DESTINATION transaction.
 *
 * We are now interested in the following: given that <s_id, sf_type> exists in
 * the Special Facility table, what is the probability that the start and end
 * time from the GET_NEW_DESTINATION transaction parameters satisfy the
 * inequality constraints with at least one of the Call Forwarding records?
 *
 * Below, we assume a given <s_id, sf_type>, and simulate the Call Forwarding
 * records that might exist for Special Facility tuple. Each of the three
 * possible Call Forwarding records with start_time 0, 8, or 16 exists with
 * probability .5.
 */

int main()
{
	int num_success = 0;

	std::vector<cf_record_t> V;

	for(int iter = 0; iter < ITERS; iter++) {
		V.clear();

		// Create the Call Forwarding records
		for(int _start_time = 0; _start_time <= 16; _start_time += 8) {
			/* Each start_time exists with probability .5 */
			bool insert = ((rand() % 2) == 0);
			if(insert) {
				/* end_time of the record is random at steady state */
				int _end_time = rand() % 24 + 1;
				V.push_back(cf_record_t(_start_time, _end_time));
			}
		}

		/* Simulate start_time and end_time from the transaction parameters */
		int probe_start_time = (rand() % 3) * 8;
		int probe_end_time = (rand() % 24) + 1;

		/*
		 * Check if any of the Call Forwarding records satisfies the inequality
		 * constraints.
		 */
		bool row_returned = false;
		for(cf_record_t cf_record: V) {
			if(cf_record.start_time <= probe_start_time && 
				probe_end_time < cf_record.end_time) {
				row_returned = true;
			}
		}

		if(row_returned) {
			num_success++;
		}
	}

	printf("Probability = %.2f\n", (float) num_success / ITERS);
}
