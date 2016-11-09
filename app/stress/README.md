# A stress test for OCC
A basic stress test for the OCC transaction protocol.
 * The test uses a table that maps 64-bit keys to 64-bit values.
 * Populate the table with `num_rows_total` keys, each mapped to 0.
 * At each iteration, pick a base key `b`, and execute one of three types of
   transactions on key set `S = {b, b + 1, ..., b + N - 1}`.
   * `get_N`: Read all keys in `S`
   * `ins_N`: Pick one random number `r`. Insert all keys in `S`, and set all
     values to `r`.
   * `del_N`: Delete all keys in `S`.
 * The main correctness check is: if a `get_N` transaction commits, it should
   have observed all `N` keys, and the values for all keys must have been
   identical.

## Important parameters
The main experiment parameters are defined in `stress.json`.
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

The configuration of MICA hash table used for the database table is in
`table.json`. The number of table rows per machine is defined in `stress_defs.h`.
The number of rows should be kept small to ensure that there plenty of conflicts
that can detect errors in the OCC implementation. The value of `N` is defined
in `worker.cc`.

## Running the benchmark
At machine `i` in `{0, ..., num_machines - 1}`, execute `./run-servers.sh i`

