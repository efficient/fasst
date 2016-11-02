sudo ipcrm -M 24

num_threads=28
sudo numactl --cpunodebind=0 --membind=0 ./main $num_threads
