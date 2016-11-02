# Calculate the total number of requests by all machines

# Create a temp file
rm cumulative_tput
touch cumulative_tput

if [ "$#" -ne 1 ]; then
    echo "Illegal number of parameters"
	echo "Usage: ./sum-reqs.sh <number of machines>"
	exit
fi

last_machine=`expr $1 - 1`

# Iterate over the machnes
for i in `seq 0 $last_machine`; do
	# If the file is not empty, select the third to last line of the file
	# that matches "Total reqs"
	if [ -s "out-machine-$i" ]; then
		tail -10 out-machine-$i | grep "Total reqs" | tail -3 | head -1 | cut -d' ' -f 8 >> cumulative_tput
	fi
done

# Sum the cumulative_tput file
echo "Total lines in cumulative_tput file = `cat cumulative_tput | wc -l`"
sum=`cat cumulative_tput | sed 's/,//g' | paste -sd+ | bc | numfmt --grouping`

echo "Total requests = $sum"
