#!/bin/bash

# Modified from Intel DPDK's tools/setup.sh

#   BSD LICENSE
#
#   Copyright(c) 2010-2013 Intel Corporation. All rights reserved.
#   All rights reserved.
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#     * Neither the name of Intel Corporation nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

disable_oom_kills()
{
	echo "Disabling OOM kills"
	sudo sysctl -q -w vm.overcommit_memory=1

	# This is OK for [8192, 8192] page configuration.
	sudo sysctl -q -w kernel.shmmax=34359738368
	sudo sysctl -q -w kernel.shmall=34359738368
}

drop_shm()
{
	echo "Dropping SHM entries"

	for i in $(ipcs -m | awk '{ print $1; }'); do
		if [[ $i =~ 0x.* ]]; then
			sudo ipcrm -M $i 2>/dev/null
		fi
	done
}

drop_cache()
{
	echo "Dropping the page cache"

	echo "echo 3 > /proc/sys/vm/drop_caches" > .echo_tmp
	sudo sh .echo_tmp
	rm -f .echo_tmp
}

remove_mnt_huge()
{
	echo "Unmounting /mnt/huge and removing directory"

	grep -s '/mnt/huge' /proc/mounts > /dev/null
	if [ $? -eq 0 ] ; then
		sudo umount /mnt/huge
	fi

	if [ -d /mnt/huge ] ; then
		sudo rm -R /mnt/huge
	fi
}

clear_huge_pages()
{
	echo "Removing currently reserved hugepages"

	echo > .echo_tmp
	for d in /sys/devices/system/node/node? ; do
		echo "echo 0 > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
	done
	sudo sh .echo_tmp
	rm -f .echo_tmp

	remove_mnt_huge
}

create_mnt_huge()
{
	echo "Creating /mnt/huge and mounting as hugetlbfs"

	sudo mkdir -p /mnt/huge

	grep -s '/mnt/huge' /proc/mounts > /dev/null
	if [ $? -ne 0 ] ; then
		sudo mount -t hugetlbfs nodev /mnt/huge
	fi
}

set_numa_pages()
{
	clear_huge_pages

	echo "Reserving hugepages"

	echo > .echo_tmp
	for d in /sys/devices/system/node/node? ; do
		node=$(basename $d)
		Pages=$1
		echo "Number of pages for $node: $Pages"
		shift
		echo "echo $Pages > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
	done
	sudo sh .echo_tmp
	rm -f .echo_tmp

	create_mnt_huge
}


disable_oom_kills
drop_shm
drop_cache

set_numa_pages $*

echo Done!
