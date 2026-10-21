CC = gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -march=native
LDFLAGS = -luring -lpthread

ifdef DEBUG
    CFLAGS = -O0 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -march=native
endif

SRCS = benchmark.c io_engine.c ../../common/src/targets.c

.PHONY: all clean

all: benchmark

benchmark: $(SRCS) io_engine.h ../../common/inc/targets.h
	$(CC) $(CFLAGS) -I../../common/inc -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f benchmark
