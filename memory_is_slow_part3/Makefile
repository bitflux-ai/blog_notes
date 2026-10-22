# Memory is slow, Disk is fast - Part 3: build every structure and the node generator.
# Needs liburing-dev and libavl-dev (Debian/Ubuntu package names).

BUILD_TYPE ?= release
DIRS = avl linkedlist sortedarray art bptree bptree2 nextarray nextarray2 diskarray create_nodes

.PHONY: all clean $(DIRS)

all: $(DIRS)

$(DIRS):
	$(MAKE) -C $@ BUILD_TYPE=$(BUILD_TYPE)

clean:
	for dir in $(DIRS); do $(MAKE) -C $$dir clean; done
