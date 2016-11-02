for i in `seq 1 70`; do
	echo "Updating NIC firmware on node-$i"
	ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cd /users/akalia/OFED; sudo ./mlnxofedinstall --fw-update-only 1>out-$i 2>out-$i &"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "ibv_devinfo | grep fw_ver"

	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "last reboot | head -1"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cd; ./setup-apt.sh"
	#ssh -oStrictHostKeyChecking=no node-$i\.RDMA.fawn.apt.emulab.net "cat /sys/devices/system/node/*/meminfo | grep Huge"
	sleep .1
done
