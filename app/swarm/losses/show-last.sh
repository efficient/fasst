# Calculate the total number of requests by all workers

if [ "$#" -ne 1 ]; then
    echo "Illegal number of parameters"
	echo "Usage: ./sum-reqs.sh <number of workers>"
	exit
fi

last_worker=`expr $1 - 1`

# Iterate over the machnes
for i in `seq 0 8 $last_worker`; do
	# If the file is not empty, select the third to last line of the file
	# that matches "Total reqs"
	if [ -s "out-worker-$i" ]; then
		echo "Worker $i"
		tail -2 out-worker-$i
	fi
done

#for i in `seq 0 8 $last_worker`; do
#	cat out-worker-$i | grep -ri Error
#done

