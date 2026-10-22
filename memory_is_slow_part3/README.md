# Memory is slow, Disk is fast - Part 3

Code and data for [Memory is slow, Disk is fast - Part 3](https://www.bitflux.ai/blog/memory-is-slow-part3/):
can an on-disk data structure beat an in-RAM binary tree, even when Big O says it shouldn't?

Every structure keeps a sorted set of 8-byte keys and supports `search(key)`, `at(i)`, `insert(key)` and
`delete(key)`. The sweep doubles the node count from 2K up to as far as each one can go (64G nodes for
DiskArray) and runs 5 million random transactions per phase.

## What's here

| Directory | What it is |
|---|---|
| `sortedarray/` | a sorted array: binary search, `memmove` to insert and delete |
| `linkedlist/` | a sorted linked list (in the sweep; the post only discusses it) |
| `avl/` | AVL tree from libavl (`libavl-dev`), with node counts for `at(i)` |
| `art/` | adaptive radix tree |
| `bptree/`, `bptree2/` | B+tree; `bptree2/batched_bench` is the pipelined version |
| `nextarray/`, `nextarray2/` | sorted extents in RAM with a directory and count trees; `nextarray2/batched_bench` is the pipelined version. `nextarray2/ARCH.md` describes the layout |
| `diskarray/` | the same design with the extents on NVMe drives, io_uring and a buffer pool sized for L3. `diskarray/PERF.md` has tuning notes |
| `create_nodes/` | `fast_nodes`: generates the one sorted key file every size reads from |
| `run/` | `run_part3.sh` (the whole sweep), `summarize.py`, `merge_runs.py`, and `results/unified/summary.txt`, the table every chart in the post comes from |
| `charts/` | `charts.py` and `diagrams.py` regenerate every figure from that summary; `zoom.html` is the "not all operations are created equal" animation |

Shared code (timing, the node generator's reader, drive targets) is in `../common`.

## Build

```sh
sudo apt install liburing-dev libavl-dev
make
```

## Run

Generate the node file first (the sweep tops out at 2^36 nodes, 512 GiB of keys; use a smaller `-c` and
`MAX_LOG2` to stay in RAM-sized runs):

```sh
(cd create_nodes && ./fast_nodes -c $((1 << 36)) -o ../data)
```

Then the in-RAM structures:

```sh
STRUCTS="avl sortedarray art bptree bptree2 nextarray nextarray2" run/run_part3.sh
```

`run/run_part3.sh` documents every knob at the top (sizes, timeouts, extent calibration, RAM limit). Each
run writes `run/results/<timestamp>/` with every command, its output and a `summary.txt`.

**DiskArray** needs the NVMe-oF lab setup from the post: 8 drives exported over RDMA, DDIO opened to all
L3 ways, and a `targets.yml` listing the drives. The script drives that through a separate tooling repo
(`WARGY`, not public) and refuses to run DiskArray without `CONFIRM=1`, because **it overwrites every drive
in `targets.yml`**.

## Charts

```sh
cd charts && uv run charts.py && uv run diagrams.py
```

They read `run/results/unified/summary.txt` and use the fonts in `charts/fonts/` (Chakra Petch, Saira and
Nixie One, from Google Fonts under the SIL Open Font License). A few numbers in the post are estimates
from the data layouts rather than measurements, for example the bytes each insert uses and moves; those
are marked in `charts.py`.

## Provenance

The published numbers come from the runs merged in `run/results/unified/` (`sources.tsv` lists which run
each data point came from), made on a 24-core Xeon Platinum 8260 with 192 GiB of DDR4 and 8 Samsung
PM983 drives over NVMe-oF. The code here is the current version. `nextarray2`'s pipeline got further fixes
after its run, so it may beat the published numbers.
