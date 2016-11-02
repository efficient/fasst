# FaSST
Fast, scalable, and simple transactions over RDMA. For historical reasons, FaSST
is called HoTS in this codebase.

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

## Required settings
More details coming soon. Every benchmark is contained in one directory.
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

# Instructions to run on Emulab's Apt cluster:
 * Set `HRD_REGISTRY_IP` to `node-1`'s IP address
 * Reduce `HRD_MAX_INLINE` to 60 bytes
 * Reduce `num_ports` to 1
 * Change the thread-to-core binding from `thread i -> core 2 * i` to
   `thread i -> core i`
