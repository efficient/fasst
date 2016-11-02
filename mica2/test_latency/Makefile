MICA_SRC := ../src
CXX := g++-5
INC	:= -I ${MICA_SRC}
#DEBUG := -DNDEBUG
LTO := -flto
CPPFLAGS := ${LTO} -O3 -std=c++11 ${DEBUG} ${INC} -Wall -Werror \
	-Wno-unused-result -Wno-unused-value -Wno-unused-function

LD := ${CXX} ${LTO}
LDFLAGS := ${LDFLAGS} -lnuma -lpapi -lpthread

APPS := main
all: ${APPS}

src := main.o

main: ${src}
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ${src} ${APPS}
