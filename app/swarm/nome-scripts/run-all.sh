echo "Removing loss records from losses/"
rm -f losses/out-worker-*

for i in `seq 0 39`; do
	node_id=`expr $i + 1`
	echo "Starting server $i on node-$node_id"
	ssh -oStrictHostKeyChecking=no node-$node_id\.RDMA.fawn.nome.nx "cd hots/app/swarm; ./run-servers.sh $i" &
done
