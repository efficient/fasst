# Print the errors
cat ../out/err-machine-* | grep "at time" | cut -d' ' -f 31 | sort | uniq
