all: rpc

HOTS_HOME := ..
MICA_HOME := ../mica2/src

INC	:= -I ${HOTS_HOME} -I ${MICA_HOME}

CXX := g++-5
# DEBUG := -DNDEBUG
CPPFLAGS := -c -O3 ${DEBUG} -std=c++11 ${INC} -Wall -Werror \
	-Wno-unused-result -Wno-unused-value -Wno-unused-function \
	-Winline

LD := ${CXX}
LDFLAGS := ${LDFLAGS} -libverbs -lrt -pthread -lmemcached -lnuma -lpapi \
	-lboost_system -lboost_coroutine

src := ${HOTS_HOME}/libhrd/hrd_conn.o ${HOTS_HOME}/libhrd/hrd_util.o \
	${HOTS_HOME}/rpc/rpc.o ${HOTS_HOME}/rpc/rpc_datapath.o

# Handle MICA differently. MICA relies on DNDEBUG to disable datapath asserts.
MICA_DEBUG := -DNDEBUG
mica_src := ${MICA_HOME}/mica/util/config.o \
	${MICA_HOME}/mica/util/cityhash/city_mod.o
${mica_src} : CPPFLAGS += ${MICA_DEBUG}

rpc: ${src} ${mica_src}

PHONY: clean
clean:
	rm -f *.o rpc ${src} ${mica_src}
