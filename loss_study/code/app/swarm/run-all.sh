echo "Removing loss records from losses/"
rm -f losses/out-worker-*
echo "Removing error records from out/"
rm -f out/*

# Maintain server_id separately because there might be gaps in chosen nodes
server_id=0

for i in `seq 1 26` `seq 28 70`; do
	node_id=$i
	echo "Starting server $i on node-$node_id"
	ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cd /users/akalia/hots/app/swarm/; ./run-servers.sh $server_id" &
	server_id=`expr $server_id + 1`

	if [ "$i" -eq 1 ]; then
		echo "Giving 2 seconds for memcached on node-1 to start"
		sleep 2
	fi
done
