# RPC performance benchmark
Measures the performance of FaSST RPCs

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
  * `req_batch_size`: Number of requests per request batch.
  * `size_req`: Size of requests (bytes).
  * `size_resp`: Size of responses (bytes).

Other parameters in `main.h`:
  * `USE_UNIQUE_WORKERS`: If enabled, each request in a batch is sent to
    a different machine. This requires `req_batch_size > num_machines`.

## Running the benchmark
At machine `i` in `{0, ..., num_machines - 1}`, execute `./run-servers.sh i`

