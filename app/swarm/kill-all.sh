# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

for i in `seq 1 70`; do
	blue "Killing node $i"
	ssh -oStrictHostKeyChecking=no akalianode-$i\.RDMA.fawn.apt.emulab.net "cd /users/akalia/hots/app/swarm/; ./kill.sh"
done
