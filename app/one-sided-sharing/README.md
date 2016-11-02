# one-sided-sharing

# Overview
 * This benchmark measures the CPU overhead of sharing QPs. In an experiment,
   `num_qps` QPs are shared by `num_threads` server threads.

 * `NUM_CLIENT_MACHINES` client machines are used to create QPs that the
   server's QPs are connected to.

# Initialization
 * The queue pairs at the server are numbered `[0, ..., num_qps - 1]`. A QP
   with global index `i` is created on port `i % num_ports` at both server
   and clients.
 * The main thread at the server creates a total of `num_qps` queue pairs across
   all ports. Each QP is created on a different port, and all QPs are used.
 * Each client machine creates `num_qps / NUM_CLIENT_MACHINES` queue pairs
   on each port. The client's QPs will remain inactive, so they do not us
   separate control blocks. The client machine supplies
   `num_qps / NUM_CLIENT_MACHINES` QPs to the global sequence of QPs
   `[0, ..., num_qps - 1]` , so some QPs on each port remain unused.

# Notes
 * This benchmark uses all signaled operations. Using unsignaled operations with
   shared QPs is complicated or impossible. To detect a READ completion by
   polling on target memory, a thread will sometimes need to poll on a different
   thread's target buffer, reducing performance. Further, we need CQEs for
   one-sided WRITEs.
 * The server creates one control block per QP. This is because the Mellanox
   driver seems to grab a per-context lock on `post_send`, and all QPs in a
   control block share the context.
 * The raw performance of the server with a single thread and a larger number of
   QPs is lower than, say, `rc-swarm`, or `rw-tput-sender` in `rdma_bench`.
   The thread achieves at most 7.8 Mrps with this benchmark; in `rc-swarm`,
   the thread can achieve around 10.7 Mrps.
    * There are two reasons for this. First, this benchmark uses more NIC
      resources by creating a separate control block per server QP. Second, it
      does not use non-signaled operations.
    * The single-threaded server throughput of this benchmark is similar to
      `rw-tput-sender`'s if (a) this benchmark's server uses one control block
      per port instead of per QP, and (b) if `rw-tput-sender` disables selective
      signalling.
