#!/bin/bash

# Execute some command on all nodes in $apt_nodes

# Request apt-params.sh to generate the list of nodes so that we can run a
# script there. The list can be in any order.
# Do not overwrite the nodemap previously generated by run-all.sh
apt_gen_nodes=1
overwrite_nodemap=0
source $(dirname $0)/apt-params.sh

for i in $apt_nodes; do
	echo "Executing check-nics.sh on node $i"

## GRUB update
	#echo "Copying grub to node $i"
	#scp /etc/default/grub akalianode-$i\.RDMA.fawn.apt.emulab.net:.

	#echo "Moving grub to /etc/default/grub and updating;"
	#ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "sudo mv grub /etc/default/grub; sudo update-grub; sudo reboot"
##

	#ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "cd ~/systemish/scripts/; sudo ./hugepages-create.sh 0 6000"
	#ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "cd ~/systemish/scripts/; ./hugepages-check.sh"
	ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "ibv_devinfo | grep PORT_"
	#ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "ps -afx | grep main"
done
