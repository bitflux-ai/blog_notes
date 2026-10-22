CC = gcc
BUILD_TYPE ?= release

ifeq ($(BUILD_TYPE),debug)
    CFLAGS = -O0 -g -Wall -Wextra -std=c99 -D_GNU_SOURCE
else
    CFLAGS = -O2 -Wall -Wextra -std=c99 -DNDEBUG -D_GNU_SOURCE
endif

LDFLAGS = -lm -lcrypto -lpthread
INCLUDES = -I../inc -I../../common/inc

TARGETS = benchmark test

.PHONY: all clean

all: $(TARGETS)

benchmark: benchmark.c art.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

test: test.c art.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS)
