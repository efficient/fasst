# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

for i in `seq 0 39`; do
	node_id=`expr $i + 1`
	blue "Killing server $i on node-$node_id"
	ssh -oStrictHostKeyChecking=no node-$node_id\.RDMA.fawn.nome.nx "cd hots/app/swarm; ./kill.sh"
done
