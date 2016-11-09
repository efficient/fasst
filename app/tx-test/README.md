# Basic transactions benchmark
A benchmark for basic multi-key read/write transactions.

## Important parameters
The main experiment parameters are defined in `tx-test.json`.
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
  * `num_locks_kilo`: Currently unused
  * `num_keys_kilo`: Average number of keys per thread in the cluster
  * `val_size`: Size of values in the key-value items
  * `zipf_theta`: The Zipfian skew parameter
  * `read_set_size`: The number of keys accessed in each transaction
  * `write_percentage`: The percentage of transaction keys (on average) that
    are written to.

The configuration of the MICA hash table used for database table is in
`fixedtable.json`. The use of Zipfian workload and latency measurement are
controlled using `USE_ZIPF` and `MEASURE_LATENCY` in `worker.cc`

## Running the benchmark
At machine `i` in `{0, ..., num_machines - 1}`, execute `./run-servers.sh i`
