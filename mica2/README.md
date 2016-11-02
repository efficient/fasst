MICA 2
======

A fast in-memory key-value store.

Requirements
------------

 * Linux x86\_64 >= 3.0
 * Intel CPU >= Sandy Bridge
 * Hugepage (2 GiB) support

Dependencies for compilation
----------------------------

 * g++ >= 5.1
 * cmake >= 2.8
 * make >= 3.80
 * libnuma-dev >= 2.0
 * libcurl-dev >= 7.35
 * DPDK >= 2.1

Dependencies for execution
--------------------------

 * bash >= 4.0
 * python >= 3.0
 * etcd >= 2.2

Compiling DPDK
--------------

         * cd dpdk-2.1.0
         * make config T=x86_64-native-linuxapp-gcc
         # it is recommended to increase "CONFIG_RTE_MEMPOOL_CACHE_MAX_SIZE" to 4096 in build/.config
         * make -j

Compiling MICA
--------------

         * cd mica2/build
         * ln -s ../../dpdk-2.1.0 ./dpdk
         * cmake ..
         * make -j

Setting up the general environment
----------------------------------

         * cd mica2/build
         * ln -s src/mica/test/*.json .
         * ../script/setup.sh 8192 8192    # 2 NUMA nodes, 16 Ki pages (32 GiB)
         * killall etcd; ../../etcd-v2.2.1-linux-amd64/etcd &

Setting up the DPDK environment
-------------------------------

         * sudo modprobe uio
         * sudo insmod dpdk/build/kmod/igb_uio.ko
         * dpdk/tools/dpdk_nic_bind.py --status
         * sudo dpdk/tools/dpdk_nic_bind.py --force -b igb_uio 0000:02:00.0 0000:02:00.1 0000:04:00.0 0000:04:00.1 0000:83:00.0 0000:83:00.1 0000:84:00.0 0000:84:00.1
         * sudo dpdk/tools/dpdk_nic_bind.py --force -b igb_uio 0000:01:00.0 0000:01:00.1 0000:03:00.0 0000:03:00.1 0000:42:00.0 0000:42:00.1 0000:43:00.0 0000:43:00.1

Running microbench
------------------

         * cd mica2/build
         * sudo ./microbench 0.00          # 0.00 = uniform key popularity

Authors
-------

Hyeontaek Lim (hl@cs.cmu.edu)

License
-------

        Copyright 2014, 2015 Carnegie Mellon University

        Licensed under the Apache License, Version 2.0 (the "License");
        you may not use this file except in compliance with the License.
        You may obtain a copy of the License at

            http://www.apache.org/licenses/LICENSE-2.0

        Unless required by applicable law or agreed to in writing, software
        distributed under the License is distributed on an "AS IS" BASIS,
        WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
        See the License for the specific language governing permissions and
        limitations under the License.

