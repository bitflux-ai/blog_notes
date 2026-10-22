#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib", "numpy"]
# ///
"""Data charts for Memory is slow, Disk is fast - Part 3 (diagrams are in diagrams.py).

    uv run charts.py            # writes *.png next to this file
"""
import matplotlib.pyplot as plt
import numpy as np

from style import (AVL_L3_NODES, D, DISK_RAM_GIB, FAINT, GROUND, INK, MUTED, OP_LABEL, OPS, RED, RED_FILL, TPS, count, frame,
                   nodes_axis, ns, rate, save, series, setup, title, tps, tps_axis)


def lines(ax, op, styles, max_nodes=None, min_nodes=None):
    for s, st in styles.items():
        x, y = series(s, op, max_nodes, min_nodes)
        ax.plot(x, y, color=st["color"], linestyle=st.get("ls", "-"), linewidth=st.get("lw", 1.6),
                marker=st.get("marker", "o"), markersize=3.5, label=st["label"], zorder=st.get("z", 3))


def grid4(name, styles, heading, max_nodes=None, lo=11, hi=36, step=5, subs=None, legend_ax=0, min_nodes=None, ysubs=(1.0,)):
    """2x2 small multiples, one per operation, transactions/s vs nodes"""
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.6), layout="constrained", sharex=True, sharey=True)
    for ax, op in zip(axes.flat, OPS):
        lines(ax, op, styles, max_nodes, min_nodes)
        nodes_axis(ax, lo, hi, step)
        tps_axis(ax, ysubs)
        frame(ax)
        title(ax, OP_LABEL[op], subs and subs[op])
    for ax in axes[:, 0]:
        ax.set_ylabel(f"{TPS} (higher is better)")
    for ax in axes[0]:
        ax.set_xlabel("")
    if legend_ax is None:
        h, l = axes.flat[0].get_legend_handles_labels()
        fig.legend(h, l, loc="outside lower center", ncol=len(h))
    else:
        axes.flat[legend_ax].legend(loc="lower left")
    fig.suptitle(heading, x=0.01, ha="left", fontweight="bold", fontsize=15, color=INK)
    save(fig, name)


# ---- tldr: AVL at its largest vs diskarray at its largest -----------------------

# diskarray's own "RAM usage" line in the 2G-node run (summary.txt's RSS is sudo's)
DISK_RAM_2G_MIB = 264


def tldr():
    """both at 2G nodes, the largest size AVL reached"""
    n = max(D["avl"]["nodes"])
    a = [tps("avl", p, n) for p in OPS]
    d = [tps("diskarray", p, n) for p in OPS]
    fig, ax = plt.subplots(figsize=(9, 4.2), constrained_layout=True)
    x, w = np.arange(len(OPS)), 0.36
    ax.bar(x - w / 2, a, w, facecolor="none", edgecolor=INK, linewidth=1.2, zorder=3,
           label="AVL TREE IN RAM")
    ax.bar(x + w / 2, d, w, facecolor=RED_FILL, edgecolor=RED, linewidth=2, zorder=3,
           label="DISKARRAY ON DISK")
    top = max(a + d)
    for xs, vs, c in ((x - w / 2, a, INK), (x + w / 2, d, RED)):
        for xi, v in zip(xs, vs):
            ax.text(xi, v + top * 0.015, rate(v), ha="center", va="bottom", color=c, fontsize=11)
    for xi, av, dv in zip(x, a, d):
        ax.text(xi + w / 2, dv + top * 0.09, f"{dv / av:.1f}x", ha="center", va="bottom", color=RED, fontsize=14,
                fontweight="bold")
    ax.set_xticks(x, [OP_LABEL[p] for p in OPS])
    ax.set_ylabel(f"{TPS} (higher is better)")
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: rate(v) if v else "0"))
    ax.set_ylim(0, top * 1.3)
    ax.legend(loc="upper right")
    frame(ax)
    title(ax, "ON DISK BEATS IN RAM AT EVERYTHING")
    save(fig, "tldr.png")


# ---- the classics: sorted array vs AVL at the small end ---------------

CLASSIC = {
    "sortedarray": dict(label="SORTED ARRAY", color=MUTED, ls="--", marker="s"),
    "avl": dict(label="AVL TREE", color=RED, lw=2.4, z=4),
}


def classic():
    grid4("classic.png", CLASSIC, "SORTED ARRAY vs AVL, 2K TO 4M NODES", max_nodes=4194305, lo=11, hi=22, step=2,
          subs={"search": "Sorted array O(log n) · AVL O(log n)",
                "index": "Sorted array O(1) · AVL O(log n)",
                "insert": "Sorted array O(n) · AVL O(log n)",
                "delete": "Sorted array O(n) · AVL O(log n)"}, legend_ax=2)


# ---- AVL vs DiskArray over the whole sweep ----------------------------------------

DUEL = {
    "avl": dict(label="AVL TREE (IN RAM)", color=INK),
    "diskarray": dict(label="DISKARRAY (ON DISK)", color=RED, lw=2.4, z=4),
}


DUEL_SUBS = {"search": "AVL O(log n) · DiskArray O(log n)", "index": "AVL O(log n) · DiskArray O(log n)",
             "insert": "AVL O(log n) · DiskArray O(√n) to O(log n)",
             "delete": "AVL O(log n) · DiskArray O(√n) to O(log n)"}


def duel():
    """split where AVL stops fitting in L3; the 512K point is in both"""
    grid4("duel_cache.png", DUEL, f"AVL FITS IN L3: 2K TO {count(AVL_L3_NODES)} NODES",
          max_nodes=AVL_L3_NODES + 1, lo=11, hi=19, step=2, subs=DUEL_SUBS, legend_ax=0, ysubs=(1, 2, 5))
    grid4("duel_dram.png", DUEL, f"AVL IN DRAM: {count(AVL_L3_NODES)} TO 64G NODES",
          min_nodes=AVL_L3_NODES, lo=19, hi=34, step=3, subs=DUEL_SUBS, legend_ax=0, ysubs=(1, 2, 5))


# ---- insert and delete as bandwidth, 2G nodes ---------------------------------------

# AVL: ~400 B it needs per insert/delete (the talk's figure); every line it touches is
# ~31 levels x 2 lines read on the way down + ~31 written on the way up, ~93 x 64 B.
# DiskArray: measured drive traffic (/proc/diskstats) at 2G nodes, read + write.
AVL_USEFUL_B, AVL_LINES_B = 400, 93 * 64
DISK_DRIVES_GIB = {"insert": 2.55 + 2.56, "delete": 2.54 + 2.54}
DRAM_ONE_CORE_GIB, FIO_8_DRIVES_GIB = 13.1, 22.6   # Part 2.5: likwid one-core DRAM read, fio 8 drives


def bandwidth():
    big = 2147483649
    ins, dele = tps("avl", "insert", big), tps("avl", "delete", big)
    rows = [  # label, GiB/s, how, style
        ("AVL INSERT", ins * AVL_USEFUL_B / 2**30, f"{rate(ins)}/s x 400 B it needs", "avl"),
        ("AVL DELETE", dele * AVL_USEFUL_B / 2**30, f"{rate(dele)}/s x 400 B it needs", "avl"),
        ("AVL INSERT, EVERY\nCACHE LINE TOUCHED", ins * AVL_LINES_B / 2**30, f"{rate(ins)}/s x ~6 KiB, at most", "bound"),
        ("DISKARRAY INSERT", DISK_DRIVES_GIB["insert"], f"{rate(tps('diskarray', 'insert', big))}/s, measured at the drives", "disk"),
        ("DISKARRAY DELETE", DISK_DRIVES_GIB["delete"], f"{rate(tps('diskarray', 'delete', big))}/s, measured at the drives", "disk"),
    ]
    fig, ax = plt.subplots(figsize=(10, 4.6), constrained_layout=True)
    y = np.arange(len(rows))[::-1]
    for yi, (lbl, v, how, kind) in zip(y, rows):
        disk = kind == "disk"
        ax.barh(yi, v, height=0.6, zorder=3, facecolor=RED_FILL if disk else "none",
                edgecolor=RED if disk else INK, linewidth=2 if disk else 1.2, linestyle="--" if kind == "bound" else "-")
        txt = f"{v * 1024:.0f} MiB/s" if v < 1 else f"{v:.1f} GiB/s"
        box = dict(facecolor=GROUND, edgecolor="none", pad=1)
        ax.text(v * 1.12, yi + 0.1, txt, va="center", color=RED if disk else INK, fontsize=12, zorder=4, bbox=box)
        ax.text(v * 1.12, yi - 0.22, how, va="center", color=MUTED, fontsize=8, zorder=4, bbox=box)
    ax.set_yticks(y, [r[0] for r in rows])
    ax.set_xscale("log")
    ax.set_xlim(0.08, 40)
    ax.set_xticks([100 / 1024, 1, 10], ["100 MiB/s", "1 GiB/s", "10 GiB/s"])
    ax.minorticks_off()
    for x, txt, ha in ((DRAM_ONE_CORE_GIB, f"ONE CORE FROM DRAM {DRAM_ONE_CORE_GIB} GiB/s ", "right"),
                       (FIO_8_DRIVES_GIB, f" 8 DRIVES, RAW {FIO_8_DRIVES_GIB} GiB/s", "left")):
        ax.axvline(x, color=MUTED, linestyle=":", linewidth=1.2, zorder=2)
        ax.text(x, y[0] + 0.5, txt, color=MUTED, fontsize=8, va="center", ha=ha)
    ax.set_xlabel("EQUIVALENT BANDWIDTH (log)")
    frame(ax, "x")
    title(ax, "INSERT AND DELETE AS BANDWIDTH, 2G NODES",
          "AVL in RAM uses about 1% of what one core can pull from DRAM. DiskArray moves real blocks.")
    save(fig, "bandwidth.png")


# ---- what one insert touches, 2G nodes ------------------------------------------------

# Estimates per insert, from the layouts (not measured, except the drive traffic):
# AVL, ~31 levels: down, each level reads the node line and the key line (62 x 64 B), of
#   which it needs a child pointer, the key pointer and the key (~24 B); up, it rewrites
#   count + height in every ancestor (~9 B each), dirtying ~31 lines that get written back.
# DiskArray: index lookup in RAM (~10 lines, ~27 comparisons x 8 B needed) plus count-tree
#   updates (~5 lines); the 4 KiB extent read and written (measured: 1 read + 1 write per
#   insert); in RAM, ~9 probes inside the block and a memmove of half its ~380 keys (DDIO
#   may leave the block in L3, but with 2048 in flight nothing guarantees it, so count it as DRAM).
LEVELS = 31
# One ledger for every per-insert byte count at 2G nodes. Two definitions, applied to both:
#   used  = bytes the algorithm deliberately reads + bytes it deliberately writes
#   moved = whole 64 B cache lines the CPU touches (DRAM), or 4 KiB blocks (NVMe)
# All estimates from the layouts except the NVMe traffic (measured: 1 read + 1 write per insert).
AVL_READ_USED = LEVELS * 24          # per level: child pointer 8 + key pointer 8 + key 8
AVL_READ_LINES = LEVELS * 2 * 64     # per level: the node's line and the key's line
AVL_WRITE_USED = LEVELS * 9          # per ancestor: count + height
AVL_WRITE_LINES = LEVELS * 64        # per ancestor: its dirty line, written back
DA_INDEX_USED = 250                  # ~27 key compares x 8 B + ~5 count updates x 8 B
DA_INDEX_LINES = 15 * 64             # top samples, directory samples, count-tree paths
DA_PROBE_USED = 9 * 8                # binary search inside the 512-key block: 9 keys
DA_PROBE_LINES = 9 * 64              # ... in 9 lines
DA_SHIFT = 190 * 8 * 2               # ~190 keys (half of ~380) read and written: all used, ~48 lines
DA_BLOCK = 4096                      # one extent
AVL_USED = AVL_READ_USED + AVL_WRITE_USED                      # 1,023
AVL_MOVED = AVL_READ_LINES + AVL_WRITE_LINES                   # 5,952
DA_USED = DA_INDEX_USED + DA_PROBE_USED + DA_SHIFT             # 3,362
DA_DRAM = DA_INDEX_LINES + DA_PROBE_LINES + DA_SHIFT           # 4,576
DA_NVME = 2 * DA_BLOCK                                         # 8,192, measured
DA_HANDLED = DA_INDEX_LINES + DA_BLOCK                         # the block it rewrites + the index it reads
INSERT_BYTES = {
    "avl": [  # (label, bytes, where, needed)
        ("keys + pointers\nit reads", AVL_READ_USED, "dram", True),
        ("rest of the 62 cache\nlines it pulls in", AVL_READ_LINES - AVL_READ_USED, "dram", False),
        ("counts + heights\nit writes", AVL_WRITE_USED, "dram", True),
        ("rest of the 31 dirty\nlines written back", AVL_WRITE_LINES - AVL_WRITE_USED, "dram", False),
    ],
    "disk_dram": [
        ("index + block\nlookups", DA_INDEX_USED + DA_PROBE_USED, "dram", True),
        ("rest of the\nlookup lines", DA_INDEX_LINES + DA_PROBE_LINES - DA_INDEX_USED - DA_PROBE_USED, "dram", False),
        ("shift ~190 keys,\nread + written", DA_SHIFT, "dram", True),
    ],
    "disk_nvme": [
        ("read the extent\nfrom the drive", DA_BLOCK, "disk", True),
        ("write the extent\nto the drive", DA_BLOCK, "disk", True),
    ],
}


def insert_bytes():
    big = 2147483649
    fig, ax = plt.subplots(figsize=(11, 5.4), constrained_layout=True)
    fills = {"dram": INK, "disk": RED}
    # one row per place the bytes go; DiskArray's insert rate goes on its NVMe row, the one that limits it
    rows = [("disk_nvme", "DISKARRAY\nNVMe", 0, "diskarray"), ("disk_dram", "DISKARRAY\nDRAM", 1.4, None),
            ("avl", "AVL TREE\nDRAM", 2.8, "avl")]
    for key, name, y, s in rows:
        x = 0
        for lbl, b, where, needed in INSERT_BYTES[key]:
            c = fills[where]
            ax.barh(y, b, left=x, height=0.55, zorder=3, edgecolor=GROUND if needed else c, linewidth=1.4,
                    facecolor=c if needed else "none", alpha=0.85 if needed else 1, hatch=None if needed else "///")
            below = b < 400          # too narrow for a label on top: put it under the bar
            ax.text(x + b / 2, y - 0.42 if below else y + 0.42, lbl, ha="center", va="top" if below else "bottom",
                    fontsize=7.5, color=MUTED, linespacing=1.0)
            ax.text(x + b / 2, y, f"{b / 1024:.1f}K" if b >= 1024 else f"{b:.0f}", ha="center", va="center",
                    fontsize=8.5, color="white" if needed else c, zorder=5,
                    bbox=None if needed else dict(facecolor=GROUND, edgecolor="none", pad=1))
            x += b
        total = x
        ax.text(total + 150, y + (0.08 if s else 0), f"{total / 1024:.1f} KiB per insert", va="center", fontsize=11, color=INK)
        if s:
            ax.text(total + 150, y - 0.2, f"{rate(tps(s, 'insert', big))} inserts/s", va="center", fontsize=9,
                    color=RED if s == "diskarray" else MUTED)
    ax.set_yticks([r[2] for r in rows], [r[1] for r in rows])
    ax.set_ylim(-0.4, 3.7)
    ax.set_xlim(0, 10600)
    ax.set_xticks([0, 2048, 4096, 6144, 8192], ["0", "2 KiB", "4 KiB", "6 KiB", "8 KiB"])
    ax.set_xlabel("BYTES MOVED PER INSERT (estimated; drive traffic measured)")
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(facecolor=INK, edgecolor=INK, label="DRAM, needed"),
                       Patch(facecolor="none", edgecolor=INK, hatch="///", label="DRAM, along for the ride"),
                       Patch(facecolor=RED, edgecolor=RED, label="NVMe drive")],
              loc="upper right", ncol=3, fontsize=8)
    frame(ax, "x")
    title(ax, "EVERYTHING ONE INSERT TOUCHES, 2G NODES",
          "AVL's bytes all come from DRAM. DiskArray's are split between DRAM and the drives.")
    save(fig, "insert_bytes.png")


# ---- what you count: four ways to compare one insert, 2G nodes ----------------------

def counting():
    rows = [  # label, AVL bytes, DiskArray bytes (the ledger above INSERT_BYTES)
        ("WHAT DISKARRAY HANDLES\nvs WHAT AVL USES", AVL_USED, DA_HANDLED),
        ("EVERYTHING MOVED,\nDRIVES INCLUDED", AVL_MOVED, DA_DRAM + DA_NVME),
        ("BYTES ACTUALLY\nUSED", AVL_USED, DA_USED),
        ("BYTES MOVED\nTHROUGH DRAM", AVL_MOVED, DA_DRAM),
    ]
    rows.sort(key=lambda r: r[2] / r[1])
    fig, ax = plt.subplots(figsize=(11, 4.8), constrained_layout=True)
    x, w = np.arange(len(rows)), 0.36
    top = max(max(a, d) for _, a, d in rows) / 1024
    for xi, (lbl, a, d) in zip(x, rows):
        ax.bar(xi - w / 2, a / 1024, w, facecolor="none", edgecolor=INK, linewidth=1.2, zorder=3,
               label="AVL TREE" if xi == 0 else None)
        ax.bar(xi + w / 2, d / 1024, w, facecolor=RED_FILL, edgecolor=RED, linewidth=2, zorder=3,
               label="DISKARRAY" if xi == 0 else None)
        ax.text(xi - w / 2, a / 1024 + top * 0.015, f"{a / 1024:.1f}", ha="center", va="bottom", fontsize=9, color=INK)
        ax.text(xi + w / 2, d / 1024 + top * 0.015, f"{d / 1024:.1f}", ha="center", va="bottom", fontsize=9, color=RED)
        ax.text(xi, top * 1.17, f"{d / a:.2g}x", ha="center", va="center", fontsize=20, color=RED if d > a else INK,
                fontweight="bold")
    ax.set_xticks(x, [r[0] for r in rows])
    ax.set_ylim(0, top * 1.3)
    ax.set_ylabel("KiB PER INSERT")
    ax.legend(loc="upper right", bbox_to_anchor=(1.0, 0.86))
    frame(ax)
    title(ax, "IS DISKARRAY DOING MORE WORK? DEPENDS WHAT YOU COUNT",
          "One insert at 2G nodes. Big number: DiskArray / AVL. Drive traffic measured, the rest estimated.")
    save(fig, "counting.png")


# ---- what limits each insert: how busy is the resource, 2G nodes ------------------

# Achieved per-insert traffic x inserts/s, against what the resource can do:
#   DRAM: 13.1 GiB/s, one core's streaming read (Part 2.5), as the yardstick for one thread
#   NVMe: ~5.5 GiB/s, the best mixed 4 KiB read+write the drives sustained in this sweep
#         (diskarray insert, 32M-1G nodes)
NVME_MIXED_GIB = 5.5


def limits():
    big = 2147483649
    avl_moved = sum(b for _, b, _, _ in INSERT_BYTES["avl"])
    disk_dram = DA_DRAM                           # the ledger above INSERT_BYTES
    a, d = tps("avl", "insert", big), tps("diskarray", "insert", big)
    rows = [  # label, achieved GiB/s, capacity GiB/s, capacity label, limiter?, note
        ("AVL TREE\nDRAM", a * avl_moved / 2**30, DRAM_ONE_CORE_GIB, "one core from DRAM", False,
         "latency-bound: 62 reads in a row, ~50 ns each"),
        ("DISKARRAY\nDRAM", d * disk_dram / 2**30, DRAM_ONE_CORE_GIB, "one core from DRAM", False,
         "index + the 4 KiB block: room to spare"),
        ("DISKARRAY\nNVMe", DISK_DRIVES_GIB["insert"], NVME_MIXED_GIB, "8 drives, mixed 4 KiB", True,
         "bandwidth-bound: the drives are nearly full"),
    ]
    fig, ax = plt.subplots(figsize=(11, 4.4), constrained_layout=True)
    y = np.arange(len(rows))[::-1] * 1.3
    for yi, (lbl, got, cap, caplbl, limiter, note) in zip(y, rows):
        c = RED if limiter else INK
        ax.barh(yi, cap, height=0.6, facecolor="none", edgecolor=c, linewidth=1.2, linestyle="--", zorder=2)
        ax.barh(yi, got, height=0.6, facecolor=c, alpha=0.85, edgecolor=GROUND, zorder=3)
        ax.text(got / 2, yi, f"{got:.1f}", ha="center", va="center", color="white", fontsize=10, zorder=5)
        ax.text(cap + 0.2, yi + 0.12, f"{got / cap:.0%} of {cap:g} GiB/s", va="center", fontsize=11, color=c)
        ax.text(cap + 0.2, yi - 0.2, f"{caplbl} · {note}", va="center", fontsize=8, color=MUTED)
    ax.set_yticks(y, [r[0] for r in rows])
    ax.set_xlim(0, 24)
    ax.set_ylim(-0.6, y[0] + 0.6)
    ax.set_xticks([0, 2, 4, 6, 8, 10, 12, 14], ["0", "2", "4", "6", "8", "10", "12", "14 GiB/s"])
    ax.set_xlabel("TRAFFIC PER SECOND DURING INSERT (filled) vs WHAT THE RESOURCE CAN DO (dashed)")
    frame(ax, "x")
    title(ax, "WHAT LIMITS EACH INSERT, 2G NODES",
          "AVL leaves DRAM mostly idle while it waits. DiskArray runs the drives nearly flat out.")
    save(fig, "limits.png")


# ---- growth: how much slower from 2K to 1G nodes vs what log n predicts ---------

def latency():
    """What one step costs, L1 to NVMe, on a log scale, with the human-scale version (L1 hit = 1 s).
    Cache and DRAM latencies are typical for this CPU (Cascade Lake, ~3.9 GHz): L1 ~4 cycles,
    L2 ~14, L3 ~70, DRAM ~85 ns. NVMe: fio, one PM983, 4 KiB random read, 87 us at queue depth 1 and
    155K reads/s at 32 deep (diskarray/PERF.md). The 'in flight' rows are cost per access, not latency,
    from the best measured rates: L3 and DRAM one core streaming 64 B lines (likwid, Part 2.5: 24.8 and
    13.1 GiB/s), NVMe DiskArray's best drive read rate in the sweep (6.24 GiB/s of 4 KiB reads)."""
    rows = [  # label, ns, kind
        ("L1 CACHE HIT", 1.0, "lat"),
        ("L2 CACHE HIT", 3.6, "lat"),
        ("L3 CACHE HIT", 18, "lat"),
        ("DRAM MISS", 85, "lat"),
        ("NVMe 4 KiB READ", 87_000, "lat"),
        ("L3, IN PARALLEL", 64 / (24.8 * 2**30) * 1e9, "flight"),
        ("DRAM, IN PARALLEL", 64 / (13.1 * 2**30) * 1e9, "flight"),
        ("NVMe, 2048 IN FLIGHT", 4096 / (6.24 * 2**30) * 1e9, "flight"),
    ]

    def human(sec):
        for div, unit in ((86400, "days"), (3600, "hours"), (60, "minutes")):
            if sec >= div:
                v = sec / div
                return f"{v:.0f} {unit if v >= 1.5 else unit[:-1]}"
        if sec < 1.05:
            return "1 second"
        return f"{sec:.1f} seconds" if sec < 10 else f"{sec:.0f} seconds"

    def took(ns_):
        return f"{ns_ / 1000:.0f} us" if ns_ >= 1000 else (f"{ns_:.1f} ns" if ns_ < 10 else f"{ns_:.0f} ns")

    fig, ax = plt.subplots(figsize=(11, 5.4), constrained_layout=True)
    y = np.arange(len(rows))[::-1].astype(float)
    y[-3:] -= 0.5                                     # a gap before the in-flight rows
    for yi, (lbl, v, kind) in zip(y, rows):
        flight = kind == "flight"
        c = RED if "NVMe" in lbl else INK
        ax.barh(yi, v - 0.5, height=0.62, left=0.5, zorder=3, facecolor="none" if flight else (RED_FILL if c == RED else c),
                edgecolor=c, linewidth=1.6 if c == RED else 1.2, hatch="///" if flight else None,
                alpha=1 if (flight or c == RED) else 0.85)
        ax.text(v * 1.25, yi, f"{took(v)}   ·   {human(v)}", va="center", fontsize=10, color=c)
    ax.set_yticks(y, [r[0] for r in rows])
    ax.set_xscale("log")
    ax.set_xlim(0.5, 5e6)
    ax.set_xticks([1, 10, 100, 1e3, 1e4, 1e5], ["1 ns", "10 ns", "100 ns", "1 us", "10 us", "100 us"])
    ax.minorticks_off()
    ax.text(0.55, y[4] - 0.75, "MANY IN FLIGHT, BEST MEASURED: COST PER ACCESS, NOT LATENCY", fontsize=8, color=MUTED, va="center")
    ax.set_xlabel("TIME PER STEP (log)  ·  and if an L1 hit took one second")
    frame(ax, "x")
    title(ax, "NOT ALL STEPS COST THE SAME",
          "One step, L1 to NVMe. Big O counts every one of these as 1.")
    save(fig, "latency.png")


def growth():
    """How much slower each op got from 512K nodes (just past L3 for AVL) to 2G, vs what Big O predicts."""
    lo, hi = 524289, 2147483649
    fig, ax = plt.subplots(figsize=(10, 4.4), constrained_layout=True)
    x, w = np.arange(len(OPS)), 0.36
    for xi, op in zip(x, OPS):
        a, d = ns("avl", op, hi) / ns("avl", op, lo), ns("diskarray", op, hi) / ns("diskarray", op, lo)
        ax.bar(xi - w / 2, a, w, facecolor="none", edgecolor=INK, linewidth=1.2, zorder=3,
               label="AVL TREE (IN RAM)" if xi == 0 else None)
        ax.bar(xi + w / 2, d, w, facecolor=RED_FILL, edgecolor=RED, linewidth=2, zorder=3,
               label="DISKARRAY (ON DISK)" if xi == 0 else None)
        ax.text(xi - w / 2, a + 0.1, f"{a:.1f}x", ha="center", va="bottom", fontsize=11, color=INK)
        ax.text(xi + w / 2, d + 0.1, f"{d:.1f}x", ha="center", va="bottom", fontsize=11, color=RED, zorder=6,
                bbox=dict(facecolor=GROUND, edgecolor="none", pad=1))
    ax.set_xticks(x, [OP_LABEL[p] for p in OPS])
    ax.set_ylim(0, 8)
    ax.set_ylabel("TIMES SLOWER")
    ax.legend(loc="upper left")
    frame(ax)
    title(ax, "HOW MUCH SLOWER",
          "From 512K nodes, just past L3, to 2G nodes. Lower is better.")
    save(fig, "growth.png")


# ---- the field, past L3: one-at-a-time and pipelined, separately ---------------------

# Every structure is well past the 35.75 MiB L3 by 4M nodes: the leanest (NextArray, B+tree)
# take ~17-21 B/node at 2G nodes (peak RSS), so L3 holds ~2M of their nodes; ART and AVL ~512K.
PAST_L3 = 4194305

PLAIN = {
    "avl": dict(label="AVL TREE", color=FAINT, ls=":", marker="."),
    "art": dict(label="ART", color=MUTED, ls="-.", marker="^"),
    "bptree": dict(label="B+TREE", color=MUTED, ls="--", marker="s"),
    "nextarray": dict(label="NEXTARRAY", color=INK, ls="-", marker="o"),
    "diskarray": dict(label="DISKARRAY (ON DISK)", color=RED, lw=2.4, z=4),
}
PIPELINED = {
    "nextarray": dict(label="NEXTARRAY, ONE AT A TIME", color=FAINT, ls=":", marker="."),
    "bptree2": dict(label="B+TREE, PIPELINED", color=MUTED, ls="--", marker="s"),
    "nextarray2": dict(label="NEXTARRAY, PIPELINED", color=INK, ls="-", marker="o"),
    "diskarray": dict(label="DISKARRAY (ON DISK)", color=RED, lw=2.4, z=4),
}


def field():
    grid4("field_plain.png", PLAIN, "ONE AT A TIME, IN RAM: 4M TO 64G NODES", legend_ax=None,
          min_nodes=PAST_L3, lo=22, hi=34, step=3, ysubs=(1, 2, 5),
          subs={"search": "", "index": "ART has no rank, so at(i) walks", "insert": "", "delete": ""})
    grid4("field_pipelined.png", PIPELINED, "PIPELINED, IN RAM: 4M TO 64G NODES", legend_ax=None,
          min_nodes=PAST_L3, lo=22, hi=34, step=3, ysubs=(1, 2, 5),
          subs={"search": "One thread, 32 transactions resolved together", "index": "", "insert": "", "delete": ""})


# ---- what limits each insert: time, traffic, effective bandwidth, 2G nodes --------------

def effective():
    big = 2147483649
    avl_moved, avl_used = AVL_MOVED, AVL_USED
    disk_moved, disk_used = DA_NVME, DA_USED
    rows = [  # name, ns per insert, moved, used, limited by, colour
        ("AVL TREE", ns("avl", "insert", big), avl_moved, avl_used,
         "LIMITED BY DRAM LATENCY: ~62 reads in a row, ~50 ns each", INK),
        ("DISKARRAY", ns("diskarray", "insert", big), disk_moved, disk_used,
         "LIMITED BY NVMe BANDWIDTH: a 4 KiB read + a 4 KiB write", RED),
    ]
    gib = lambda b, t: b / (t * 1e-9) / 2**30
    fig, ax = plt.subplots(figsize=(11, 4.6), constrained_layout=True)
    ylab, yt = [], []
    for i, (name, t, moved, used, why, c) in enumerate(rows):
        y0 = (1 - i) * 2.4
        for dy, kind, b, fill in ((0.45, "TRAFFIC", moved, False), (-0.45, "EFFECTIVE", used, True)):
            v = gib(b, t)
            ax.barh(y0 + dy, v, height=0.7, zorder=3, facecolor=(RED_FILL if c == RED else c) if fill else "none",
                    edgecolor=c, linewidth=1.6 if fill else 1.2, alpha=1 if c == RED or not fill else 0.85)
            txt = f"{v * 1024:.0f} MiB/s" if v < 1 else f"{v:.1f} GiB/s"
            ax.text(v + 0.08, y0 + dy + 0.08, txt, va="center", fontsize=12, color=c, fontweight="bold" if fill else "medium")
            ax.text(v + 0.08, y0 + dy - 0.24, f"{b / 1024:.1f} KiB {'used' if fill else 'moved'} / {t / 1000:.1f} us per insert",
                    va="center", fontsize=8, color=MUTED)
            ylab.append(f"{name}\n{kind}")
            yt.append(y0 + dy)
        ax.text(0.0, y0 + 1.05, f"{why} · {rate(1e9 / t)} inserts/s", fontsize=8.5, color=c, va="center")
    ax.set_yticks(yt, ylab)
    ax.set_xlim(0, 7)
    ax.set_ylim(-1.3, 3.8)
    ax.set_xticks(range(0, 7), ["0"] + [f"{g} GiB/s" for g in range(1, 7)])
    frame(ax, "x")
    title(ax, "WHAT LIMITS EACH INSERT, 2G NODES",
          f"Bytes / time per insert. DiskArray moves more per insert, and moves all of it faster: "
          f"{gib(rows[1][2], rows[1][1]) / gib(rows[0][2], rows[0][1]):.1f}x the traffic, "
          f"{gib(rows[1][3], rows[1][1]) / gib(rows[0][3], rows[0][1]):.1f}x the useful data.")
    save(fig, "effective.png")


if __name__ == "__main__":
    setup()
    tldr()
    classic()
    duel()
    bandwidth()
    insert_bytes()
    counting()
    effective()
    latency()
    growth()
    field()
