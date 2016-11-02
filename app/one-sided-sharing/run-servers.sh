#!/bin/bash
source $(dirname $0)/../../util/helpers.sh
source common_params.sh

export MLX5_SINGLE_THREADED=0
export HRD_REGISTRY_IP="10.113.1.47"
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

if [ "$#" -ne 0 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-servers.sh"
	exit
fi

# With link-time optimization, main exe does not get correct permissions
chmod +x main
drop_shm

blue "Reset server QP registry"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

num_threads=3

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_threads \
	--num-qps $num_qps \
	--base-port-index $base_port_index \
	--num-ports $num_ports \
	--numa-node $numa_node \
	--use-uc $use_uc \
	--is-client 0 \
	--size 8 \
	--window-size 32 \
	--do-read 1 &
