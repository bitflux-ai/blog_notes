# DiskArray performance notes

Measurements taken 2026-10-01 on the Xeon Platinum 8260 initiator (24 cores,
35.75 MiB L3) against NVMe-oF targets over RDMA. All bandwidth figures are
GiB/s. Drive traffic is measured, not modelled: `batched_bench` reads
`/proc/diskstats` for its devices at each phase boundary.

## Setup

- Data: 1 G nodes (8 GiB of 8-byte keys) from `nodes-F37FB6DD-11.8GiB.bin`,
  pre-sorted list `nodes-sorted-F37FB6DD-8.00GiB-original.bin`, 5 M
  operations per phase with random keys.
- Drives: the 7 Samsung 1.7 TiB targets in `targets.yml`. The eighth target,
  a 465 GiB WD, was excluded for the sweeps; with it included the 8-drive
  baseline was within a few percent.
- Structure: 4 KiB extents (512 nodes), overprovisioning 1.5, dextents
  striped round-robin over the drives, one io_uring per worker, each worker
  on one drive, consumer pinned to cpu 0 with its hyperthread idle.
- Raw drive latency from fio, one drive, 4 KiB random reads: 87 µs at queue
  depth 1 (11.5 K IOPS), 155 K IOPS at queue depth 32.

## Headline

| op | AVL in DRAM | nextarray in DRAM | diskarray, batched, 7 drives |
|---|---|---|---|
| search | 1686 ns | 644 ns | 1090 ns |
| index | 2833 ns | 143 ns | 990 ns |
| insert | 4333 ns | 808 ns | 1660 ns |
| delete | 4293 ns | 830 ns | 1640 ns |
| RAM for the structure | about 64 GiB | 16 GiB | about 270 MiB |

AVL numbers are the August 2026 sweep on the same machine and node pool;
nextarray is the 2026-10-01 version. DiskArray's row is the defaults after
the driver changes described below. It runs
out of a 24 MiB buffer pool with the data on the drives (the 270 MiB is
mostly the per-extent tracking arrays for 2.8 M extents), and in batched
mode it beats AVL in DRAM on every operation. The comparison is throughput against latency: AVL and
nextarray are measured one operation at a time, diskarray keeps about 2000
operations in flight. Any single diskarray operation still waits roughly a
drive round trip.

## Engine

The worker transport was rewritten from the design in
`memory_is_slow_part2/diskbased`:

- Per-worker single-producer/single-consumer request and completion rings
  with C11 atomics. Fields written by the main thread and fields written by
  the worker sit on separate cache lines, so neither side writes a line the
  other spins on. The old scheme bumped a `volatile` counter on every worker
  from the main loop and had the workers scanning a shared array for work.
- Each worker keeps a private table of in-flight operations indexed by its
  local buffer number. A buffer has at most one I/O outstanding, so the
  table slot is unique for the life of the operation. Keying slots by issue
  order was tried first and corrupted state under out-of-order completion.
- Only the main thread touches dextent pools. The old code had the worker
  freeing dextents into a ring the main thread was allocating from.
- Striping: dextent d lives on drive d mod drives and is served by worker
  d mod workers; the worker count is rounded up to a multiple of the drive
  count. Files and buffers are registered with io_uring,
  `IORING_SETUP_SINGLE_ISSUER`, no `SQPOLL`.
- The buffer pool and the large per-extent arrays are on 2 MiB huge pages.
- The allocator waits for every worker to report ready instead of sleeping.

Single-drive throughput did not change, as expected when one drive is the
limit; the engine's gain is that it stripes. 16 M nodes, 24 workers, 12288
buffers, batch 1024:

| | old engine, 1 drive | new engine, 1 drive | new engine, 8 drives |
|---|---|---|---|
| search | 368 K op/s | 337 K op/s | 481 K op/s |
| insert | 168 K op/s | 168 K op/s | 366 K op/s |
| delete | 169 K op/s | 174 K op/s | 356 K op/s |

A latent bug surfaced along the way: `map_extent` gave up waiting for a free
buffer after a fixed 300 000 spins. That was about a second with the old
code and tens of milliseconds with the new one, less than a SATA file needs
to finish 84 outstanding writes, so the plain-file test failed half the
time. The wait is now time-based (30 s). The drives never showed it.

## Driver and command-queue changes (later the same day)

Three changes to the batched path, all keeping the single consumer thread:

- **O(1) command completion.** `diskarray_cmd_wait` used to scan the pending
  ring for the finished command. Commands complete in queue order, so the
  scan always matched on its first compare and this changed nothing
  measurable; it is simply correct by construction now.
- **Extent resolved once.** `queue_cmds` already found each command's extent
  to prefetch it; execution then found it again. The command now carries the
  extent (and the offset, for `_at`) with a generation counter, and falls
  back to a fresh lookup only if the structure changed in between.
- **Sliding window.** `batched_bench` keeps two half-batches in flight and
  requeues one while the other executes, so the drive queues never drain
  between batches. On its own this raised search and index by about 30 % but
  cut insert and delete by 20 %: steady prefetch pressure starved the pool of
  clean buffers, and dirty ones were only written back when `map_extent`
  found nothing free. The dirty-buffer watermark existed but was never acted
  on. `queue_cmds` now flushes at the watermark, so writebacks ride along with
  the batch's reads.

Result at the defaults (28 workers, 6144 buffers, batch 2048, 4 KiB):

| op | before | after | drive traffic |
|---|---|---|---|
| search | 714 K op/s, 1.40 µs | 919 K op/s, 1.09 µs | 3.50 GiB/s |
| index | 773 K op/s, 1.29 µs | 1.01 M op/s, 0.99 µs | 3.9 GiB/s |
| insert | 503 K op/s, 1.99 µs | 601 K op/s, 1.66 µs | 4.58 GiB/s |
| delete | 505 K op/s, 1.98 µs | 611 K op/s, 1.64 µs | 4.6 GiB/s |

Insert and delete now move 2.3 GiB/s of reads plus 2.3 GiB/s of writes,
about 165 K IOPS per drive. Validated with `-d 1` against the oracle files
at 1 M nodes striped.

The batch peak did not move with the flush in place: 2048 stays best
(3072 gives 582 K insert op/s, 4096 with 12288 buffers 515 K, 4096 with 6144
buffers still starves at 295 K, since the window keeps two half-batches in
flight and that is two thirds of the pool).

## Sweeps at 4 KiB extents

(These rows predate the driver changes above; the knob conclusions still hold,
the absolute numbers are about 20 % higher now.)

Insert phase, 1 G nodes, 7 drives. Delete tracks insert within 1 % in every
row; search runs about 1.4x the insert rate.

| workers | buffers | batch | insert op/s | drive traffic |
|---|---|---|---|---|
| 14 | 24576 | 2048 | 371 K | 2.81 |
| 21 | 12288 | 1024 | 443 K | 3.38 |
| 21 | 12288 | 4096 | 420 K | 3.20 |
| 21 | 49152 | 4096 | 394 K | 2.98 |
| 28 | 12288 | 512 | 386 K | 2.94 |
| 28 | 6144 | 512 | 397 K | 3.03 |
| 28 | 3072 | 512 | 400 K | 3.05 |
| 28 | 12288 | 1024 | 458 K | 3.49 |
| 28 | 24576 | 1024 | 453 K | 3.44 |
| 28 | 8192 | 1024 | 467 K | 3.56 |
| 28 | 6144 | 1024 | 471 K | 3.59 |
| 42 | 12288 | 1024 | 461 K | 3.51 |
| 42 | 6144 | 1024 | 468 K | 3.57 |
| 28 | 12288 | 2048 | 480 K | 3.66 |
| 28 | 8192 | 2048 | 492 K | 3.75 |
| **28** | **6144** | **2048** | **500 K** | **3.81** |
| 28 | 9216 | 3072 | 468 K | 3.57 |
| 28 | 12288 | 4096 | 445 K | 3.38 |
| 28 | 6144 | 4096 | 92 K | 0.70 |
| 28 | 24576 | 8192 | 394 K | 2.99 |

The best row was repeated and came back within 0.5 %. The dirty-flush
threshold (`-p`, 50 versus 90) made no difference.

What each knob does:

- **Batch size** is the main lever. It sets how many extents are in flight
  across the drives. 2048 is the peak: 512 costs 15 %, 4096 costs 12 % at
  the same buffer ratio, 8192 more again. Beyond about 290 commands per
  drive the queues gain nothing and the consumer pays for a larger pool.
- **Batch must stay well under the buffer count.** Every queued command
  prefetches one extent into a buffer. When the batch approaches the pool
  size, buffers are reclaimed before their command runs, the operation falls
  back to a synchronous load, and throughput collapses (92 K op/s in the
  4096-on-6144 row). A batch of about a third of the buffers is safe.
- **Buffers** matter little once they cover the batch. 6144, a 24 MiB pool
  that fits in the L3, is 3 % ahead of 12288 and 4 % ahead of 24576. The
  DDIO argument from the diskbased benchmark applies but is a small effect
  here because the drive round trip dominates each operation.
- **Workers** saturate at four per drive. 42 equals 28; 21 is 3 % lower and
  14 is 15 % lower.

## Extent size

21 workers, 12288 buffers, batch 1024:

| extent | insert op/s | insert drive traffic | search drive traffic |
|---|---|---|---|
| 4 KiB | 443 K | 3.38 | 2.56 |
| 8 KiB | 366 K | 5.57 | 4.57 |
| 16 KiB | 261 K | 7.90 | 6.98 |
| 32 KiB | 139 K | 8.35 | 8.52 |

At 4 KiB the drives are IOPS-bound: 500 K insert op/s is one read and one
copy-on-write write per op, a million IOPS across 7 drives, about 140 K per
drive, which is where fio puts a single drive at queue depth 32. Larger
extents turn that IOPS ceiling into bandwidth. The 7 drives top out near
8 to 8.5 GiB/s of mixed read and write, about 1.2 GiB/s each, which 16 KiB
extents already reach. 32 KiB adds nothing on insert and halves delete
(93 K op/s), because the consumer thread is then shifting 16 KiB of nodes
per operation. The project stays at 4 KiB extents; the 16 KiB row is the
bandwidth headline if one is wanted.

## Where the time goes at the best 4 KiB point

Per operation, 2.0 µs of wall time with about 2000 in flight:

- The drives: one 4 KiB read for every op and one 4 KiB write for every
  insert or delete, at roughly 140 K IOPS per drive. This is the limit.
- The consumer core: find the extent via the max index (cache-resident,
  one line per level), map the buffer, a binary search and a memmove of
  about 1 KiB inside the 4 KiB extent, a seven-level lookup-tree update
  with one vector add per level. It is not the bottleneck at 4 KiB, which
  is why more workers and more buffers stop helping, but it becomes one at
  32 KiB.
- Bulk load writes the 8 GiB data set across the drives in 3.5 s.

## Correctness checks used

- `test` on a plain file, on one drive, and striped over all targets; passes
  in all three.
- `benchmark -d 1` dumps the structure after load, insert and delete and
  byte-compares each against the pre-sorted oracle files; passes striped.
- The per-op `benchmark` on one drive runs at 72 µs per search, which is a
  single drive round trip (fio: 87 µs at queue depth 1 including the fio
  overhead), confirming there is no hidden per-op cost in the unbatched path.

## Reproduce

```
cd diskarray && make
sudo ./batched_bench -i ../data/nodes-F37FB6DD-11.8GiB.bin \
    -l ../data/nodes-sorted-F37FB6DD-8.00GiB-original.bin \
    -t ../memory_is_slow_part2/diskbased/targets.yml
```

The defaults are the best point above (28 workers, 6144 buffers, batch 2048,
4 KiB extents). `sweep.py` runs the size sweep with the same settings. To
exclude a target, copy `targets.yml` and delete its subsystem block; the
benchmark skips file-backed targets on its own. Before writing to any drive,
check what `targets.yml` resolves to: earlier the same day two entries listed
as Samsungs resolved to 465 GiB devices and one of them carried a filesystem.
