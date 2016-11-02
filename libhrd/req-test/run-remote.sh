# Action: The server uses this script to run clients remotely
shm-rm.sh 1>/dev/null 2>/dev/null

num_threads=10			#processes per client machine

echo "Running $num_threads client threads"

sudo -E ./main $num_threads 1>/dev/null 2>/dev/null &

# When we run this script remotely, the client processes die when this script dies.
# So, sleep.
sleep 10000
