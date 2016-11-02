CXX := gcc-5
INC	:= -I ../../libhrd -I ../../rpc -I ../../util -I ../../
CPPFLAGS := -O3 -std=c++11 -Wall -Werror -Wno-unused-result -Wno-unused-value -pthread ${INC}
LD := ${CXX} -O3
LDFLAGS := ${LDFLAGS} -libverbs -lrt -pthread -lmemcached -lnuma

APPS := main

all: ${APPS}

main: ../../libhrd/hrd_conn.o ../../libhrd/hrd_util.o \
	main.o worker.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ../../libhrd/*.o ${APPS}
