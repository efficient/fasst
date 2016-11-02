# A function to echo in blue color
function blue() {
	#es=`tput setaf 4`
	#ee=`tput sgr0`
	echo "${es}$1${ee}"
}

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-servers.sh <machine_number>"
	exit
fi

export HRD_REGISTRY_IP="node-1.RDMA.fawn.nome.nx"
export MLX5_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

# The 0th server hosts the QP registry
if [ "$1" -eq 0 ]; then
	blue "Resetting QP registry"
	sudo killall memcached
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1
fi

num_server_threads=4
blue "Starting $num_server_threads server threads"

if [ "$num_server_threads" -ge 8 ]; then
	blue "Fix the CPU core map below for more than 8 threads"
fi

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_server_threads \
	--machine-id $1 \
	--base-port-index 0 \
	--num-ports 1 \
	--size 60 \
	--postlist 16 1>/dev/null 2>/dev/null &

sleep 1000000
