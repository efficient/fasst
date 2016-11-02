for i in `seq 1 70`; do
	echo "Checking status of node-$i"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "ibv_devinfo | grep state"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "ibv_devinfo | grep fw_ver"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "ibv_devinfo | grep fw_ver"

	ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "uptime --since"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "ps -afx | grep main"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cd; ./setup-apt.sh"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cat /sys/devices/system/node/*/meminfo | grep Huge"
	sleep .1
done
