# TATP benchmark

## Important parameters
The main experiment parameters are defined in `tatp.json`.
  * `num_coro`: Number of coroutines per worker thread
  * `base_port_index`: The 0-based index of the first RDMA port to use
  * `num_ports`: Number of RDMA ports to use starting from `base_port_index`
  * `num_qps`: Number of SEND queues per thread. Can improve performance when
     there are a small number of cores.
  * `postlist`: The maximum number of packets sent using one Doorbell by the
     RPC subsystem.
  * `numa_node`: The NUMA node to run all threads on.
  * `num_machines`: Number of machines in the cluster.
  * `workers_per_machine`: Number of threads per machine.
  * `num_backups`: Number of backup partitions per primary partition.
  * `use_lock_server`: Currently unused.

The configuration of MICA hash tables used for database tables are in the
`tatp_json` directory. The number of Subscribers in TATP is specified in
`tatp_defs.h`.

## Running the benchmark
At machine `i` in `{0, ..., num_machines - 1}`, execute `./run-servers.sh i`

## Notes on TATP
To the best of our understanding, the official TATP benchmark description is
vague/incorrect at several places. The issues are related to the probability
distribution of records in the Call Forwarding table.

### Basic observations
 * The probability distribution of records in the Subscriber, Access Info, and
   Special Facility tables does not change.
 * The probability that a particular subscriber has an Access Info record with
   a given `ai_type` is .625. The number of Access Info records for a subscriber
   is uniformly distributed between 0 and 4.
    * If the subscriber has all 4 records, `ai_type` is found with probability 1.
    * If the subscriber has 3 records, `ai_type` is found with probability
      3/4.
    * If the subscriber has 2 records, `ai_type` is found with probability
      2/4.
    * If the subscriber has 1 record, `ai_type` is found with probability
      1/4.
   The total probability is (4 + 3 + 2 + 1) / 16 = .625
 * Similarly, the probability that a particular subscriber has a Special Facility
   record of a given `sf_type` is .625.
 * Using the above two observations, we can see that the percentage of succesful
   `GET_ACCESS_DATA` and `UPDATE_SUBSCRIBER_DATA` transactions is 62.5%.

### Call Forwarding table distribution
 * The distribution of records in the Call Forwarding table changes over time.
   Initially, the population is such that for a given `<s_id, sf_type>` record
   in the Special Facilty table, the **number** of Call Forwarding records is
   uniformly distributed between 0 and 3, each with a distinct `start_time`.
   This distribution is not maintained during the `INSERT_CALL_FORWARDING` and
   `DELETE_CALL_FORWARDING` transactions.
 * As `<s_id, sf_type, start_time>` records are randomly inserted and deleted
   during the above two transactions, at steady state, for a given
   `<s_id, sf_type>` record in the Special Facility table, the probability that
   the record `<s_id, sf_type, start_time>` exists in the Call Forwarding table
   is .5. This is because the last transaction to modify this record was either
   `INSERT_CALL_FORWARDING` or `DELETE_CALL_FORWARDING`, with equal probability.
   If it was an insertion, the record will be found. If it was a deletion, the
   record won't be found.
 * Using the above two observations we can see that the percentage of succesful
   `INSERT_CALL_FORWARDING` and `DELETE_CALL_FORWARDING` transactions is 31.25%.
   These transactions pick a random `<s_id, sf_type, start_time>` tuple. The
   probability that `<s_id, sf_type>` exists in the Special Facility table is
   .625. The probability that for a given `<s_id, sf_type>` in the Special
   Facility table, `<s_id, sf_type, start_time>` exists in the Call Forwarding
   table at steady state is .5. The combined probability is `.625 * .5 = .3125`.

### `GET_NEW_DESTINATION` success rate
Although we can reason about the success probability of all other transactions,
it's unclear how the official benchmark arrives at the 23.9% success rate for
`GET_NEW_DESTINATION`. The parameters of this transaction are
`{s_id, sf_type, start_time, end_time}`. The probability that `<s_id, sf_type>`
exists in the Special Facility table with `is_active = 1` is `.625 * .85`.
If it exists, each of the three `<s_id, sf_type, start_time = {0, 8, 16}>`
records exists in the Call Forwarding table with probability .5. Existing
records have a random `end_time` at steady state (i.e., the initial population
constraint that `end_time = start_time + (rand() % 8 + 1)` is broken during
steady state).

We are now interested in the following: given that `<s_id, sf_type>` exists in
the Special Facility table, what is the probability that the start and end time
from the `GET_NEW_DESTINATION` transaction parameters satisfy the inequality
constraints with at least one of the Call Forwarding records? This is difficult
to compute intuitively, so the code in `analysis/analysis.cc` computes it using
simulation. It comes to .38.

So, the probability that `GET_NEW_DESTINATION` succeeds is
`.635 * .81 * .38 = .201`, not equal to the required .239.

### Number of coroutines
HoTS uses RECV queues with 2048 entries, and aims to support a cluster with 100
nodes. To support `x` worker coroutines and `n` outstanding requests per
worker coroutine, the following inequality must be satisfied (see
`required_recvs()` function in `rpc.cc`):
	```
	(x * n) + (100 * x) <= 2048, or
	x <= 2048 / (n + 100)
	 ```
For TATP, `n = 4` (see transaction profiles in `worker.cc`), so `x <= 19`. The
value of `num_coro` in `tatp.json` is therefore set to 20 (`num_coro` includes
the master coroutine).
