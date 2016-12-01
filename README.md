# FaSST
Fast, scalable, and simple transactions over RDMA. For historical reasons, FaSST
is called HoTS in this codebase. This is the source code for our
[USENIX OSDI paper](http://www.cs.cmu.edu/~akalia/doc/osdi16/fasst_osdi.pdf).


## Required hardware and software
 * InfiniBand HCAs
    * RoCE HCAs have not been tested, but should work with minor modifications.
      See `is_roce()` usage in [HERD](https://github.com/efficient/HERD) for
      details.
 * Ubuntu 12.04+
 * Mellanox OFED 2.4+
 * memcached, libmemcached-dev, libmemcached-tools
 * libnuma-dev (numactl-devel on CentOS)
 * Boost 1.6 (earlier and newer versions not tested)
   * Depends on python-dev and libbz2-dev
 * libpapi-dev
 * gcc-5.3 (C++11 support required; gcc-4.8 works but can have ~10% lower performance)

## Required settings
All benchmarks run in a symmetric setting with multiple machines (i.e., there
are no designated server or client machines. Every benchmark is contained in one
directory.
 * Benchmark parameters are contained in a json file in the experiment directory.
 * Modify `HRD_REGISTRY_IP` in `run-servers.sh` to the IP address of server 0's
   machine. This machine runs a memcached instance that is used as a queue pair
   registry.
 * Allocate hugepages on the NIC's socket at the server machine. On our machines,
   the NIC is attached to socket 0, so all benchmark scripts bind allocated
   memory and threads to socket 0. On Ubuntu systems, create 8192 hugepages on
   socket 0 using:
```	
	sudo echo 8192 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

## Code structure

| Directory | Description |
| ------------- | ------------- |
| `libhrd` | An RDMA management and communication library. |
| `rpc` | FaSST RPCs. This should depend only on `libhrd` and `hots.h`. |
| `mica2` | A C++ implementation of a MICA hash table. |
| `datastore` | Interface of FaSST datastores, and RPC handlers for MICA.|
| `tx` | The transaction protocol. |
| `logger` | The commit record logger for transactions. |
| `mappings` | Helper functions to determine primary/backup/log machine numbers. |
| `app` | Benchmarks (TATP, SmallBank, RPC microbenchmark, packet loss test). |
| `drivers` | Modified `libmlx4` and `libmlx5` drivers with the cheap RECV posting optimization.|
| `loss\_study` | The code and output logs used in our InfiniBand packet loss study. |
| `other` | Contains a DCT benchmark. |

## Building the benchmarks
Each benchmark in the `app` directory contains a Makefile to build the benchmark.

## Instructions to run on Emulab's Apt cluster:
 * Set `HRD_REGISTRY_IP` to `node-1`'s IP address
 * Reduce `HRD_MAX_INLINE` to 60 bytes
 * Reduce `num_ports` to 1
 * Change the thread-to-core binding from `thread i -> core 2 * i` to
   `thread i -> core i`

## Acknowledgements
Hyeontaek Lim's MICA code is used directly and in a modified way.

## Contact
Anuj Kalia (akalia@cs.cmu.edu)

## License
		Copyright 2016, Carnegie Mellon University

        Licensed under the Apache License, Version 2.0 (the "License");
        you may not use this file except in compliance with the License.
        You may obtain a copy of the License at

            http://www.apache.org/licenses/LICENSE-2.0

        Unless required by applicable law or agreed to in writing, software
        distributed under the License is distributed on an "AS IS" BASIS,
        WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
        See the License for the specific language governing permissions and
        limitations under the License.

