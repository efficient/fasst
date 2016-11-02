#!/bin/bash
source $(dirname $0)/../../util/helpers.sh

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SHUT_UP_BF=0
export MLX5_SINGLE_THREADED=1
export MLX4_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-servers.sh <machine_number>"
	exit
fi

# With link-time optimization, main exe does not get correct permissions
chmod +x main
drop_shm

# The 0th server hosts the QP registry
if [ "$1" -eq 0 ]; then
	blue "Resetting QP registry"
	sudo killall memcached
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1
fi

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --membind=0 --cpunodebind=0 ./main --machine-id $1 &
