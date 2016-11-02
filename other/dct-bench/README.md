# DCT notes
These notes may be helpful if you are considering using DCT.

## Benchmark implementation
`libhrd` does not support DCT, so this benchmark implements its own queue pair
initialization logic.

The measurements below were done with HoTS commit `09ec57b`.

## DC target multiplexing with DCRs
Creating one DC target per machine is enough. When DC initiators connect to
the DC target, the NIC internally creates a DC responder context (DCR) for each
connected initiator. This means that one DC target is sufficient to avoid
dynamic connect/disconnect *if* the initiators never change the target that
they are connected to.

For example, if threads 1 through 10 issue READs to the same DC target on
machine 0, no dynamic connections will be created or destroyed at steady state.
When the DC target on machine 0's NIC gets the first connect packet from thread
`i`, it allocates a DCR for that thread, which will be used for subsequent
reliable communication with that thread.

These DCRs consume NIC memory and can cause cache thrashing. The cache thrashing
can be seen using the DMA read PCIe counter in an incast experiment where
many threads on remote machines issue READs to machine 0. The experiment uses
10 remote machines, 14 threads per machine, and 32 outstanding READs per machine.
Each READ fetches a 32-byte chunk from a random address on machine 0, so
the average number of cache lines read over PCIe at machine 0 per READ is 1.5.
 * **Case 1**: 1 QP per thread. This achieves 94.6 Mrps of inbound READs/s,
   and the PCIe DMA read counter rate is 141 M/s. The expected counter rate is
   142 M/s, so there are no cache misses.
 * **Case 2**: 16 QPs per thread. Only 20.1 Mrps of inbound READs can be
   achieved. In this case, there are `140 * 16` DCRs in the server's NIC, and
   the PCIe DMA read counter rate is 41 M/s. The contribution of cache misses
   to the DMA read counter rate is therefore `41 - (20.1 * 1.5) = 10.85` M/s.

## Single-thread throughput
I could achieve only up to 7.2 million outbound READs/s with one thread.
Throughput is not affected significantly if mulitple queues are used. This is
noticeably lower than 10.9 Mrps reported in the paper with RC in the
`one-sided-sharing` benchmark. Similar to `dct-bench`, `one-sided-sharing` does
not use unsignaled operations.

It's unclear to me why single-threaded throughput with DCT is low. One possible
reason could be that DCT WQEs use two cachelines; writing these WQEs to
BlueFlame with MMIO consumes more CPU cycles than RC READ WQEs. However,
`ud-sender` (from `rdma_bench`), whose WQEs also require 2 cache lines,
achieves > 10 million SENDs/s with non-batched SENDs.

## NIC throughput with static connectivity
I achieved 39 million READs/s per machine in a symmetric experiment with 8
machines and static connectivity between initiators and targets. Each machine
ran 14 threads; each thread issued all operations to one fixed DCT target, i.e.,
no dynamic connect/disconnect packets were sent.

This is lower than ~50 million READ/s per machine achievable on CIB (as reported
in our OSDI paper). I'm pretty certain that the difference is due to the larger
DC WQEs. This means that, even if dynamic connections are somehow avoided, DCT
cannot outperform FaSST RPCs (up to 40.9 Mrps).

## NIC throughput with dynamic connectivity
This experiment was done with 5 machines on CIB and 14 threads per machine.
Each thread chose the destination machine for each READ randomly. The number
of outstanding READs maintained by each thread (`window_size`) and the number
of QPs used by each thread (`num_qps`) was varied, and the average per-machine
READ rate and latency was measured.

```
window_size    num_qps    READs/machine (M/s)	Average latency (us)
16             1          9.14                  24.4
16             2          13.8                  16.2
16             4          22.0                  10.2
16             6          22.9                  9.7
16             8          22.6                  9.8
16             16         20.0                  11.2

32             1          9.1                   49.0
32             2          13.7                  32.8
32             4          20.1                  22.3
32             6          21.9                  20.4
32             8          22.1                  20.3
32             16         20.9                  21.3
32             32         18.4                  24.3
```

## Latency
To measure the latency overhead of dynamic connect and disconnect packets, we
use one thread on machine #0 that issues READs to remote DCT targets. In scheme
1, the same remote DCT target is chosen for every READ. In scheme 2, the DCT
target chosen for a READ is always different from the previous READ's target.

**Case 1**: The thread maintains one outstanding READ.
  * Scheme 1 (READ from previously-connected DCT target): 1.9 us
  * Scheme 2 (READ from a different DCT target): 2.9 us

This means that the latency overhead of one dynamic connect and disconnect cycle
is pretty low (~1 us). However, in practice, where a thread must keep multiple
READs outstanding, the latency overhead is very high (discussed below).

**Case 2**: The thread maintains a window of 16 outstand READs
  * Scheme 1 (READ from previously-connected DCT target): ~4 us
  * Scheme 2 (READ from a different DCT target): 26 us
The average latency in Case 2 with Scheme 2 is much higher than Case 1 with
Scheme 2. This is because READs issued by a thread get queued locally for several
RTTs while previous READs are processed. In Case 2, consider the instant when a
thread issues the 16th READ. If RC transport was being used, the NIC could
immediately place this READ on the wire. In DC transport, however, the NIC
needs to wait until the DC initiator is free, which happens only after READs
1 through 15 are all finished.
