# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}


blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

num_server_threads=1
#num_client_machines=1
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
# This ssh needs to be synchronous
ssh -oStrictHostKeyChecking=no $HRD_REGISTRY_IP "memcflush --servers=localhost"

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E ./main -t $num_server_threads -c 0 &

exit

#
#sleep 1
#
#for i in `seq 1 $num_client_machines`; do
#	blue "Starting client $client_id"
#	mc=`expr $i + 1`
#	client_id=`expr $mc - 2`
#	ssh -oStrictHostKeyChecking=no node-$mc.RDMA.fawn.apt.emulab.net "
#		cd mica-intel/ib-dpdk; 
#		./run-remote.sh $client_id" &
#	sleep .5
#done
