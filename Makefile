LIB=libiomp.a

CC=cc
CFLAGS=-std=c99 -g -Wall -pipe -fPIC -fvisibility=hidden -D_BSD_SOURCE
CXX=c++
CXXFLAGS=-std=c++11 -g -Wall -pipe
AR=ar
ARFLAGS=rc
LD=c++
LDFLAGS=-lpthread

all: $(LIB) test test_unit

.PHONY: clean
clean:
	rm -f $(LIB) iomp_log.o iomp.o iomp_kqueue.o iomp_epoll.o test test.o test_unit test_unit.o

rebuild: clean all

$(LIB): iomp_log.o iomp.o iomp_kqueue.o iomp_epoll.o
	$(AR) $(ARFLAGS) $@ iomp_log.o iomp.o iomp_kqueue.o iomp_epoll.o

test: test.o $(LIB)
	$(LD) -o $@ test.o -L. -liomp $(LDFLAGS)

iomp_log.o: iomp_log.c
	$(CC) -c $(CFLAGS) -o $@ $<

iomp.o: iomp.c
	$(CC) -c $(CFLAGS) -o $@ $<

iomp_kqueue.o: iomp_kqueue.c
	$(CC) -c $(CFLAGS) -o $@ $<

iomp_epoll.o: iomp_epoll.c
	$(CC) -c $(CFLAGS) -o $@ $<

test_unit: test_unit.o $(LIB)
	$(LD) -o $@ test_unit.o -L. -liomp $(LDFLAGS)

test.o: test.cc
	$(CXX) -c $(CXXFLAGS) -o $@ $<

test_unit.o: test_unit.cc
	$(CXX) -c $(CXXFLAGS) -o $@ $<

