#!/bin/bash
source $(dirname $0)/../../util/helpers.sh
source common_params.sh

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-machine.sh <machine-id>" 
	exit
fi

# With link-time optimization, main exe does not get correct permissions
chmod +x main
drop_shm

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--machine-id $1 \
	--num-qps $num_qps \
	--base-port-index $base_port_index \
	--num-ports $num_ports \
	--numa-node $numa_node \
	--use-uc $use_uc \
	--is-client 1 &
