## Packet loss test
Important parameters:
 * `NUM_WORKERS`: Number of threads in the cluster
 * `num_server_threads`: Number of threads in a server
 * `PER_WORKER_CREDITS`: Number of outstanding requests a thread can have to
   any other thread. This must be more than one to detect packet loss or
   reordering.
 * `CHECK_PACKET_LOSS`: This enables detection of packet loss and reordering.
   By disabling this option, `swarm` can be used as a request-reply over UD verbs
   performance benchmark.

## Running the experiment
The experiment needs `K = NUM_WORKERS / num_server_threads` machines. At each
machine `i` in `{0, ..., K - 1}`, run `./run-servers.sh $i`.
