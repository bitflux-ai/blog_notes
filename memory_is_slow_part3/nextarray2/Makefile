CC = gcc
BUILD_TYPE ?= release

ifeq ($(BUILD_TYPE),debug)
    CFLAGS = -O0 -g -Wall -Wextra -std=c99 -D_GNU_SOURCE
else
    CFLAGS = -O2 -Wall -Wextra -std=c99 -DNDEBUG -D_GNU_SOURCE
endif
# The benchmarks' nodes are 8-byte integer keys: inlined compare and a
# branchless lower_bound (nextarray.h); the generic path is still there for
# other node sizes.
CFLAGS += -DNEXTARRAY_U64_KEYS

LDFLAGS = -lm -lpthread -lcrypto
INCLUDES = -I../inc -I../../common/inc

TARGETS = benchmark test batched_bench

.PHONY: all clean

all: $(TARGETS)

benchmark: benchmark.c nextarray.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

batched_bench: batched_bench.c nextarray.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) -lpthread

test: test.c nextarray.c ../../common/src/benchmark_helpers.c ../../common/src/gen_nodes.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS)
