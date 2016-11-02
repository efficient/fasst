#!/bin/bash

# Request apt-params.sh to generate a new randomized list of nodes.
# Save the random order to the nodemap.
apt_gen_nodes=1
overwrite_nodemap=1
source $(dirname $0)/apt-params.sh

rm -rf $apt_logdir
mkdir $apt_logdir

# Maintain server_id separately because there might be gaps in chosen nodes
server_id=0

for i in $apt_nodes; do
	echo "Starting machine-$server_id on node $i"
	ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net \
		"cd /users/akalia/hots/app/$apt_app/; ./run-remote.sh $server_id" &

	server_id=`expr $server_id + 1`

	if [ "$i" -eq 1 ]; then
		echo "Giving 2 seconds for memcached on node 1 to start"
		sleep 2
	fi
done
