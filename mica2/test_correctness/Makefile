MICA_SRC := ../src
CXX := g++-5
INC	:= -I ${MICA_SRC}
#DEBUG := -DNDEBUG
CPPFLAGS := -O3 -std=c++11 ${DEBUG} ${INC} -Wall -Werror \
	-Wno-unused-result -Wno-unused-value -Wno-unused-function
LD := ${CXX} -O3
LDFLAGS := ${LDFLAGS} -lnuma -lpapi -lpthread

APPS := test_table
all: ${APPS}

src := ${MICA_SRC}/mica/util/config.o \
	${MICA_SRC}/mica/util/cityhash/city_mod.o \
	test_table.o

test_table: ${src}
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ${src} ${APPS}
