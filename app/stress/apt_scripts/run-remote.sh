#!/bin/bash
source $(dirname $0)/../../util/helpers.sh

# We don't need the node list. We only need apt_logdir.
apt_gen_nodes=0
overwrite_nodemap=0
source $(dirname $0)/apt-params.sh

export HRD_REGISTRY_IP="128.110.96.96"
export MLX5_SINGLE_THREADED=1
export MLX4_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

if [ "$#" -ne 1 ]; then
    echo "Illegal number of parameters"
	echo "Usage: ./run-servers.sh <machine_number>"
	exit
fi

# With link-time optimization, main exe does not get correct permissions
chmod +x main
drop_shm

# $apt_logdir must be created at each node (run-all.sh cannot do this)
rm -rf $apt_logdir
mkdir $apt_logdir

# The 0th server hosts the QP registry
if [ "$1" -eq 0 ]; then
	echo "Resetting QP registry"
	sudo killall memcached
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1
fi

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --membind=0 ./main \
	--machine-id $1 1>$apt_logdir/out-machine-$1 2>$apt_logdir/err-machine-$1 &

sleep 10000000
