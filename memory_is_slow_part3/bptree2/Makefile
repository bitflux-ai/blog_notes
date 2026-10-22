CC = gcc
BUILD_TYPE ?= release

ifeq ($(BUILD_TYPE),debug)
    CFLAGS = -O0 -g -Wall -Wextra -std=c99 -D_GNU_SOURCE
else
    CFLAGS = -O2 -Wall -Wextra -std=c99 -DNDEBUG -D_GNU_SOURCE
endif
# The benchmarks' nodes are 8-byte integer keys: inlined compare and
# branchless searches (bptree.c); the generic path is still there for other
# node sizes.  The group resolve that batched_bench uses needs them.
CFLAGS += -DBPTREE_U64_KEYS

LDFLAGS = -lm -lcrypto -lpthread
INCLUDES = -I../inc -I../../common/inc

TARGETS = benchmark test batched_bench

.PHONY: all clean

all: $(TARGETS)

benchmark: benchmark.c bptree.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

batched_bench: batched_bench.c bptree.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

test: test.c bptree.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS)
