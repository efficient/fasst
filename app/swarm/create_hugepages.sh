for i in `seq 1 26` `seq 28 70`; do
	echo "Creating hugepages on node $i"
	ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "cd /users/akalia/systemish/scripts/; ./hugepages-create.sh 0 6000"
	sleep .1
done
