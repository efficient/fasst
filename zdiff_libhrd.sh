# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "hrd.h:"
diff ~/rdma_bench/libhrd/hrd.h libhrd/hrd.h

blue "hrd_conn.c:"
diff ~/rdma_bench/libhrd/hrd_conn.c libhrd/hrd_conn.cc

blue "hrd_util.cc:"
diff ~/rdma_bench/libhrd/hrd_util.c libhrd/hrd_util.cc
