# nextarray2: helper-thread experiments

Question (2026-10-02): can insert and delete on the in-memory nextarray run
out of warm cache, with one thread still doing all the mutation, by having
other threads do the memory-latency-bound work ahead of it, the way DDIO
feeds the diskbased benchmark's consumer from L3?

Short answer: the only thing that works while keeping all the work on the
single thread is pipelining inside that thread. Resolving operations in
groups of 16 to 32 with the misses of each index level and each probe in
flight together, then prefetching each operation's memmove range and
write-back path a few operations ahead, cuts insert and delete by a third
and search by half at 1 G nodes, with no other threads.

Helpers on other cores only pay when they do the search themselves and hand
back positions (insert and delete halved, search 5x with four helpers), which
moves the algorithm off the single thread and is a different design. Helpers
that only touch memory on the consumer's behalf, the direct analogue of the
diskarray I/O workers, do nothing measurable at 1 G: on this CPU no other
core can place lines in the L3 for the consumer, and the hyperthread that can
reach its L2 costs more than it saves.

## The driver

`batched_bench` is a batched single-consumer loop. The consumer on cpu 0
mutates; everything else only reads. Modes (`--prefetch`):

- `none`: the batched loop with no help. Baseline.
- `self`: the consumer issues prefetches for operations `--window` ahead.
- `helper`: a thread (default: cpu 0's hyperthread sibling) pulls each
  upcoming extent's used bytes into cache, by prefetch instruction or demand
  load (`-L`), optionally limited (`-b`), strided (`-s`), hinted (`-t`).
- `search`: `-n` helper threads on separate cores, dealt operations
  round-robin, each resolving groups of `-g` operations at once: top-level
  binary search, then one cache line per max-index level, then one probe
  per round of the in-extent binary search, with the group's misses
  prefetched together so `-g` misses overlap. The helper touches the lines
  the operation will use and hands back (extent, position, generation). The
  consumer checks the generation counter and the position's two neighbours
  and calls `nextarray_insert_at` / `nextarray_delete_at`, which do only the
  memmove and the bookkeeping. Anything stale falls back to the full
  operation. `-W` adds a sibling thread that pulls each resolved tail into
  the shared L2 just before execution; `-S` has the consumer prefetch it
  itself.

The library gained `nextarray_locate*`, generation counters, `*_in` and
`*_at` entry points for this. Validation: `-d 1` dumps the structure after
load, inserts and deletes and byte-compares it with the sorted oracle files;
every configuration below passes.

## Results at 1 G nodes

5 M random operations per phase, ns per operation. Helpers on cpus 2, 3, …;
consumer on cpu 0.

| configuration | search | index | insert | delete |
|---|---|---|---|---|
| none (baseline) | 619 | 229 | 791 | 826 |
| helper on sibling, window 16, demand loads | 569 | 292 | 775 | 796 |
| search, 1 helper, window 512, group 32 | 382 | 201 | 493 | 496 |
| search, 1 helper on the sibling, window 64, group 16 | 407 | 319 | 591 | 583 |
| **search, 2 helpers, window 512, group 32** | 209 | 100 | **420** | **405** |
| search, 3 helpers, window 1024, group 32 | 152 | 67 | 431 | 424 |
| **search, 4 helpers, window 1024, group 32** | **121** | **54** | 429 | 419 |
| search, 2 helpers + sibling warmer, 16 ahead | 215 | 179 | 509 | 510 |
| search, 4 helpers + sibling warmer, 32 ahead | 183 | 192 | 515 | 510 |
| search, 2 helpers + consumer self-prefetch, 2 ahead | 211 | 106 | 442 | 436 |
| search, 4 helpers + consumer self-prefetch, 4 ahead | 125 | 135 | 463 | 461 |

Fallbacks to the full operation: 10 to 1300 per 20 M, from splits or two
operations hitting one extent inside the window. Negligible.

## Single-thread pipeline (`-p pipeline`)

The same level-at-a-time trick with no helper threads: the consumer resolves
a group of operations itself, misses of each level in flight together, then
executes the group while prefetching each upcoming operation's memmove range
`-D` operations ahead and, once the extent is known, the lookup-tree path and
max entry the write-back will touch.

1 G nodes, ns per op:

| configuration | search | index | insert | delete |
|---|---|---|---|---|
| none (baseline) | 619 | 229 | 791 | 826 |
| pipeline, group 32, tails 4 ahead, no update-path prefetch | 310 | 142 | 573 | 561 |
| **pipeline, group 32, tails 4 ahead** | 315 | 144 | **526** | **518** |
| **pipeline, same, index walk also grouped (final)** | 333 | **129** | 554 | 543 |

Run-to-run spread on the pipeline rows at 1 G is about 5 % (insert measured
526 to 569 across the day); treat the pipeline as roughly 330 / 130 / 540 /
540 ns.
| search helpers, 2 cores (for reference) | 200 | 99 | 416 | 403 |

16 M nodes:

| configuration | search | index | insert | delete |
|---|---|---|---|---|
| none (baseline) | 462 | 68 | 577 | 601 |
| pipeline, group 32, tails 4 ahead | 253 | 53 | 424 | 420 |
| pipeline, group 32, no tail prefetch | 249 | 52 | 471 | 461 |
| search helpers, 2 cores | 200 | 55 | 352 | 327 |

Where the pipeline's time goes at 1 G (rdtsc split, which itself adds
about 20 ns per op):

| phase | resolve group | issue prefetches | execute |
|---|---|---|---|
| search | 92 % | 2 % | 3 % |
| index | 57 % | 5 % | 34 % |
| insert | 58 % | 20 % | 13 % |
| delete | 58 % | 20 % | 13 % |

Execute, the memmove plus tree and max updates, is about 70 ns of a 545 ns
insert: the prefetches do make it warm. What remains is the resolve itself,
about 300 ns per op even with 32 operations' misses issued together, and
about 100 ns of issuing the 20-odd prefetches per op. The resolve is slower
than its 15 memory round trips per group would suggest because 32 outstanding
prefetches exceed the core's 12 line-fill buffers, so each round is two or
three batches of misses rather than one. That is also why the separate-core
helpers win: they bring more fill buffers.

Group size at 1 G (with the rdtsc split enabled, which adds about 20 ns):

| group | search | index | insert | delete |
|---|---|---|---|---|
| 8 | 375 | 187 | 610 | 606 |
| 12 | 343 | 191 | 576 | 569 |
| 16 | 333 | 194 | 547 | 551 |
| 24 | 332 | 202 | 543 | 546 |
| 32 | 338 | 198 | 550 | 546 |
| 48 | 339 | 198 | 550 | 548 |

Matching the group to the 12 L1 fill buffers is the wrong target: 8 and 12
are clearly worse, the knee is at 16 to 24 and it is flat beyond. The
outstanding-miss budget that matters is the L2 superqueue, 32 entries on
Skylake-SP, which the L1 fill buffers drain into; prefetches past the
twelfth queue there rather than being dropped. The compares per level are
serial regardless of group size, so larger groups add nothing once the
superqueue is full.

One thread, no rings, no other cores: insert and delete 33 to 37 % under
the baseline at 1 G, search halved. The rolling tail prefetch is worth about
10 % on insert, the update-path prefetch another 8 %; four operations of
lookahead is the knee, eight is slightly worse as the prefetches start to
crowd the fill buffers. The helpers stay ahead on insert and delete by
about 110 ns, which is the resolve work the pipeline does on the consumer
and the helpers do elsewhere, and far ahead on search and index, which are
nothing but that work.

## Results at 16 M nodes

| configuration | search | index | insert | delete |
|---|---|---|---|---|
| none (baseline) | 476 | 60 | 572 | 598 |
| helper on sibling, window 16, demand loads | 363 | 98 | 493 | 501 |
| search, 1 helper on sibling, window 64, group 16 | 310 | 138 | 496 | 493 |
| search, 1 helper on separate core, window 256, group 16 | 308 | 95 | 449 | 447 |

## What the numbers say

**Moving the search chain off the consumer is the win.** Resolving the
extent and the position is a chain of dependent misses: the bottom of the
max index, then eight or so probes inside the extent. One helper pipelining
32 operations keeps 32 of those chains in flight; the consumer then does
only a compare, a memmove and a seven-line tree update. Search and index
scale with helpers because they are nothing but that chain: four helpers
give 121 ns and 54 ns, 5x and 4x the baseline. Insert and delete halve.

**Separate cores beat the hyperthread sibling for this.** The sibling can
reach the consumer's L2, but it shares the core's issue slots and its 12
line-fill buffers, and the search helper is compute-heavy as well as
miss-heavy. On a separate core it costs the consumer nothing. The index
column shows the sibling's price directly: 319 ns with the helper on the
sibling against 201 on a separate core, for identical work.

**Insert and delete stop at about 420 ns because of the memmove, and nothing
got it warm.** The tail an insert shifts, 1.1 KiB on average, costs about
35 ns from L2 and 230 to 350 ns from DRAM (measured separately). The lines
a separate-core helper touched sit in that core's L2 and the consumer
fetches them cross-core at about DRAM cost, so the helper's touch neither
helps nor hurts. Three ways of getting them into the consumer's own cache
were tried and all lost:

- A warmer thread on the sibling, pulling the resolved tail a few operations
  ahead: insert 420 to 510, index 54 to 190. The hyperthread's presence
  costs the consumer more than the memmove it saves.
- The consumer prefetching the next operations' tails itself: 420 to 442 or
  worse. The prefetches compete with the memmove for the same fill buffers.
- The whole-extent prefetch-only helper on the sibling (the first design):
  2 to 5 % at 1 G, 20 % at 16 M, and it doubles DRAM traffic.

So at 1 G the consumer's floor on this hardware is roughly: cross-core
memmove 250 ns plus bookkeeping, about 420 ns, reached with two helpers.

**Why the L3 route is closed.** The L3 on Cascade Lake is a victim cache.
A core cannot deposit lines there for another core to find; they go to L3
only when evicted from the first core's L2, and a cross-core snoop costs
about as much as DRAM. DDIO gets lines into L3 because the NIC writes them
there; a core has no equivalent until `cldemote`. Non-temporal prefetch
hints from a separate core made things worse. This is the same wall the
sibling-only design hit from the other side.

## Against the rest of the table at 1 G

| | search | index | insert | delete |
|---|---|---|---|---|
| AVL in DRAM (libavl, August) | 1686 | 2833 | 4333 | 4293 |
| nextarray, August | 842 | 209 | 1112 | 1279 |
| nextarray, today, one thread | 644 | 143 | 808 | 830 |
| nextarray2, 2 search helpers | 209 | 100 | 420 | 405 |
| nextarray2, 4 search helpers | 121 | 54 | 429 | 419 |
| diskarray batched, 8 NVMe-oF drives | 1090 | 990 | 1660 | 1640 |

The structure is still mutated by exactly one thread. The helpers read.

## Warming the L3 from other cores (`-p warm`)

The last variant keeps the spirit of the diskarray design: helpers on
separate cores run the same locate/search/touch reads `-w` operations ahead
but return nothing, purely to pull the lines the consumer will need into the
shared L3, while the consumer runs the single-thread pipeline unchanged.
`-E` makes each helper stream junk through its own L2 after every group, to
force the lines it read to be evicted into the L3 rather than sit in its L2.

1 G nodes, ns per op:

| configuration | search | index | insert | delete | DRAM read, whole run |
|---|---|---|---|---|---|
| pipeline alone | 327 | 205 | 558 | 552 | 71.1 GiB |
| warm, 2 helpers, window 2048 | 317 | 132 | 547 | 542 | |
| warm, 2 helpers, window 2048, evict 1 MiB | 329 | 161 | 594 | 597 | 84.5 GiB |
| warm, 2 helpers, window 4096, evict 2 MiB | 333 | 170 | 554 | 550 | |
| warm, 4 helpers, window 4096, evict 1 MiB | 319 | 163 | 540 | 526 | |

Insert, delete and search are unchanged within noise. The memory-controller
counters say why: the helpers' reads add 13 GiB of DRAM traffic and the
consumer's misses do not go away, so the lines the helpers read are not
being served to the consumer from L3. Forcing the helper's L2 to evict made
it worse, not better. Index improved by about a third, probably because the
lookup-tree lines are read repeatedly by both sides and some stay resident,
but that path was not the target.

**Why index improved and nothing else did.** Two reasons, and they were
separated with two more runs. First, index was the one path the pipeline did
not overlap: positional lookups were resolved one at a time with a plain
dependent walk of the lookup tree, so any cheaper miss showed up one for
one, while the grouped key paths were already hiding their latency.
Grouping the tree walk the same way (`group_locate_index`) takes index from
205 to 142 ns on its own, with no helpers. Second, the lookup-tree lines are
reused, a leaf line serving eight extents and a parent sixty-four, and the
L3's replacement policy retains lines that show reuse, so the helpers' reads
of tree lines plausibly do get kept; extent lines are touched once and are
not. With the index walk grouped:

| configuration | search | index | insert | delete |
|---|---|---|---|---|
| pipeline, grouped index walk | 343 | 142 | 569 | 572 |
| + 2 warm helpers, full reads | 340 | 134 | 580 | 569 |
| + 2 warm helpers, index walk only | 355 | 134 | 570 | 565 |

The residual index gain from warming is 6 %, and index-only warming equals
full warming on insert and delete, so the helpers' extent reads contributed
nothing at all.

This closes the question the experiment started with. On this CPU another
core cannot stage extent lines in L3 for the consumer, whether by prefetch
instruction, demand load, non-temporal hint, or forced eviction; only lines
with reuse of their own survive there. The only helper that can move memory
for the consumer is one that shares its L2, the hyperthread sibling, and
that one costs the consumer more than it saves. DDIO is a different
mechanism: the NIC writes into L3 directly, and no core-side instruction on
Cascade Lake does the same.

## Knobs and their effect

- `-n` helpers: 2 saturates insert/delete, 4 saturates search/index.
- `-g` group: 32 is the knee; 16 costs 5 % on insert, 64 adds nothing.
- `-w` window: 512 for 2 helpers, 1024 for 4; larger adds only fallbacks.
- `-H` first helper cpu: anything but 0 and its sibling 24.
- `-W`, `-S`, and `-p helper`: measured losses or near-zero at 1 G; kept
  for reproduction.
- `-p pipeline -g 32 -D 4`: the single-thread option; `-U` turns off the
  update-path prefetch (an 8 % loss on insert).

## Reproduce

```
cd nextarray2 && make
./batched_bench -p none -i ../data/nodes-F37FB6DD-11.8GiB.bin -l ../data/nodes-sorted-F37FB6DD-8.00GiB-original.bin
./batched_bench -p search -H 2 -n 2 -w 512 -g 32  -i ... -l ...
./batched_bench -p search -H 2 -n 4 -w 1024 -g 32 -i ... -l ...
```

Check `pgrep batched_bench` first: a stray run left on cpus 0 or 24 skews
everything by 2 to 3x, which happened once during this work.
