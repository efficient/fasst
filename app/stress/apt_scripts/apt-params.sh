num_nodes=60
down_node_list="43"	# List of down nodes

# Check that node 1 is not down. This node hosts the memcached registry.
if [[ $(echo $down_node_list | grep "\b1\b") ]]; then
    echo "apt-params: Error. Node 1 is in down_node_list. This is not allowed."
	exit
fi

#
# Creates a randomized list of physical nodes that:
# a. Includes all nodes from 1 through $num_nodes, except those in
#    $down_node_list
# b. The first node is "1"
#
function gen_random_nodes() {
	num_down_nodes=`echo $down_node_list | wc -w`
	num_up_nodes=`expr $num_nodes - $num_down_nodes`

	# Create a rand list of all nodes (both up nodes and down) excluding node 1
	apt_nodes=`seq 2 $num_nodes | shuf`

	# Remove the down nodes from the list
	for down_node in $down_node_list; do
		apt_nodes=`echo $apt_nodes | sed "s/\b$down_node\b//g"`
	done

	# Append node 1 to the beginning of the list
	apt_nodes=1" "$apt_nodes

	# Sanity check
	apt_nodes_size=`echo $apt_nodes | wc -w`
	if [ "$apt_nodes_size" -ne "$num_up_nodes" ]; then
		echo "apt-params: Sanity check failed"
		exit
	fi

	# Save the mapping to a file to know which node hosts which HoTS machine-id
	if [ $overwrite_nodemap -eq 1 ]; then
		rm -rf nodemap
		touch nodemap
		server_id=0
		for i in $apt_nodes; do
			echo "machine-$server_id -> akalianode-$i" >> nodemap
			server_id=`expr $server_id + 1`
		done
	fi

	echo $apt_nodes
}

# Generate nodes only if the sourcing script set apt_gen_nodes
if [ $apt_gen_nodes -eq 1 ]; then
	apt_nodes=`gen_random_nodes`
fi

apt_app="stress"	# The app to run
apt_logdir="/tmp/$apt_app/"	# Output directory at each machine
apt_combined_logdir="/tmp/$apt_app-combined" # Cumulative output collected here
