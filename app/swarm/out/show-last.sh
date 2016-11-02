# Print the last line in each out-worker-* file that is not empty
if [ "$#" -ne 1 ]; then
    echo "Illegal number of parameters"
	echo "Usage: ./show-last.sh <number of workers>"
	exit
fi

last_worker=`expr $1 - 1`
# Iterate over the workers
for i in `seq 0 $last_worker`; do
	# Print the last line if the file is not empty
	if [ -s "out-worker-$i" ]; then
		echo "Worker $i: `tail -1 out-worker-$i`"
	fi
done

# To compute the total number of requests generated:
# ./show-last.sh <num workers> | cut -d' ' -f 10 | sed 's/,//g' | paste -sd+ | bc | numfmt --grouping
