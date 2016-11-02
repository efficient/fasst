CXX := g++-5
CPPFLAGS  := -O3 -std=c++11 -Wall -Werror -Wno-unused-result -I ../../
LD      := ${CXX} -O3
LDFLAGS := ${LDFLAGS} -libverbs -lrt -lpthread -lmemcached -lnuma

APPS    := main

all: ${APPS}

main: ../../libhrd/hrd_conn.o ../../libhrd/hrd_util.o main.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ../*.o ${APPS}
