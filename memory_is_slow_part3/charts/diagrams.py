#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib", "numpy"]
# ///
"""Diagrams for Memory is slow, Disk is fast - Part 3: the structures, drawn small.

    uv run diagrams.py          # writes *.png next to this file
"""
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle

from style import FAINT, GROUND, INK, MUTED, RED, RED_FILL, TRACE, save, setup

PINK = "#f9dcdc"   # RED_FILL on GROUND, opaque, for shapes that sit on top of lines

KEYS = [3, 8, 12, 17, 21, 26, 30, 34, 39, 42, 47, 51, 55, 60, 64, 71]


def canvas(w, h, xlim, ylim):
    fig, ax = plt.subplots(figsize=(w, h), constrained_layout=True)
    ax.set_xlim(*xlim)
    ax.set_ylim(*ylim)
    ax.set_aspect("equal")
    ax.axis("off")
    return fig, ax


def cell(ax, x, y, text, w=1, h=1, edge=INK, fill="white", color=INK, lw=1.4, fs=11, dashed=False):
    ax.add_patch(Rectangle((x, y), w, h, facecolor=fill, edgecolor=edge, linewidth=lw, zorder=3,
                           linestyle="--" if dashed else "-"))
    ax.text(x + w / 2, y + h / 2, str(text), ha="center", va="center", color=color, fontsize=fs, zorder=4)


def arrow(ax, a, b, color=INK, lw=1.4, rad=0.0, style="-|>", ls="-", z=5):
    ax.add_patch(FancyArrowPatch(a, b, arrowstyle=style, mutation_scale=12, color=color, linewidth=lw,
                                 connectionstyle=f"arc3,rad={rad}", linestyle=ls, zorder=z, shrinkA=2, shrinkB=2))


def label(ax, x, y, text, color=MUTED, fs=9, ha="left", va="center", weight="medium"):
    ax.text(x, y, text, color=color, fontsize=fs, ha=ha, va=va, fontweight=weight, zorder=6)


def heading(ax, x, y, text):
    label(ax, x, y, text, color=INK, fs=12, weight="bold")


# ---- sorted array: search, at(), insert, delete -----------------------------------

def row(ax, y, keys, hot=(), moved=(), empty=(), gone=(), x0=0):
    """one array of len(keys) slots; None is an empty slot"""
    for i, k in enumerate(keys):
        if i in gone:
            cell(ax, x0 + i, y, "", edge=RED, fill=GROUND, dashed=True)
        elif k is None or i in empty:
            cell(ax, x0 + i, y, "", edge=FAINT, fill=GROUND, dashed=True)
        elif i in hot:
            cell(ax, x0 + i, y, k, edge=RED, fill=PINK, color=RED, lw=2)
        elif i in moved:
            cell(ax, x0 + i, y, k, edge=MUTED, fill="white", color=INK)
        else:
            cell(ax, x0 + i, y, k)


def sorted_search():
    fig, ax = canvas(10, 4.0, (-0.5, 17), (-2.4, 4.4))
    heading(ax, 0, 4.0, "SEARCH(42): THREE HOPS")
    probes = [7, 11, 9]           # 34 < 42 -> right; 51 > 42 -> left; 42 found
    row(ax, 0, KEYS, hot=probes)
    for i in range(16):
        label(ax, i + 0.5, -0.35, str(i), fs=8, ha="center")
    # hop 1 drops in from above the middle; hops 2 and 3 arc between probes
    arrow(ax, (7.5, 3.0), (7.5, 1.05), color=RED, lw=1.8)
    label(ax, 7.5, 3.3, "1", color=RED, fs=12, ha="center", weight="bold")
    arrow(ax, (7.6, 1.05), (11.4, 1.05), color=RED, lw=1.8, rad=-0.5)
    label(ax, 9.5, 2.35, "2", color=RED, fs=12, ha="center", weight="bold")
    arrow(ax, (11.4, -0.6), (9.6, -0.6), color=RED, lw=1.8, rad=-0.6)
    label(ax, 10.5, -1.45, "3", color=RED, fs=12, ha="center", weight="bold")
    label(ax, 12.6, 3.3, "1: middle is 34, too small, go right\n2: middle of the rest is 51, too big, go left\n3: 42, found", color=RED, fs=9)
    save(fig, "sorted_search.png")


def sorted_at():
    fig, ax = canvas(10, 2.8, (-2.5, 17), (-1.6, 3.0))
    heading(ax, -2, 2.6, "AT(5): NO SEARCH, ONE ACCESS")
    row(ax, 0, KEYS, hot=(5,))
    for i in range(16):
        label(ax, i + 0.5, -0.35, str(i), fs=8, ha="center")
    label(ax, -2, 0.5, "base", color=INK, fs=10)
    arrow(ax, (-1.1, 0.5), (0, 0.5), color=INK)
    arrow(ax, (0.5, -0.7), (5.5, -0.7), color=RED, lw=1.8, rad=0.3)
    label(ax, 6.2, -1.2, "address = base + 5 x 8 bytes", color=RED, fs=9)
    save(fig, "sorted_at.png")


def stage(ax, y, n, text):
    label(ax, -0.4, y + 0.5, f"{n}", color=INK, fs=12, ha="right", weight="bold")
    label(ax, 16.4, y + 0.5, text, fs=9)


def shift(ax, y, a, b, color=MUTED):
    """one arrow over a block of cells that moved"""
    arrow(ax, (a, y + 1.35), (b, y + 1.35), color=color, lw=1.6)


def sorted_insert():
    fig, ax = canvas(10, 3.8, (-1.2, 21), (-0.4, 6.4))
    heading(ax, -1.0, 6.1, "INSERT(23)")
    start = KEYS[:15] + [None]
    stage(ax, 4.6, 1, "start")
    row(ax, 4.6, start)
    opened = KEYS[:5] + [None] + KEYS[5:15]
    stage(ax, 2.3, 2, "open a gap:\neverything after\nit moves right")
    row(ax, 2.3, opened, moved=range(6, 16))
    shift(ax, 2.3, 6.2, 15.8)
    stage(ax, 0, 3, "write 23")
    row(ax, 0, KEYS[:5] + [23] + KEYS[5:15], hot=(5,))
    save(fig, "sorted_insert.png")


def sorted_delete():
    fig, ax = canvas(10, 3.8, (-1.2, 21), (-0.4, 6.4))
    heading(ax, -1.0, 6.1, "DELETE(42)")
    stage(ax, 4.6, 1, "start")
    row(ax, 4.6, KEYS, hot=(9,))
    stage(ax, 2.3, 2, "remove 42")
    row(ax, 2.3, KEYS[:9] + [None] + KEYS[10:], gone=(9,))
    stage(ax, 0, 3, "close the gap:\neverything after\nit moves left")
    row(ax, 0, KEYS[:9] + KEYS[10:] + [None], moved=range(9, 15))
    shift(ax, 0, 15.8, 9.2)
    save(fig, "sorted_delete.png")


# ---- linked list: cheap to splice, slow to find -----------------------------------

def linked_list():
    fig, ax = canvas(10, 4.4, (-0.5, 19), (-3.0, 4.6))
    heading(ax, 0, 4.2, "LINKED LIST: EACH NODE IS A KEY AND A POINTER TO THE NEXT ONE, ANYWHERE IN RAM")
    xs = [0, 3, 6, 9, 12, 15]
    ys = [1.2, 0.0, 1.6, 0.4, 1.4, 0.2]
    keys = [3, 8, 12, 17, 26, 30]
    for x, y, k in zip(xs, ys, keys):
        cell(ax, x, y, k, w=1.2)
        cell(ax, x + 1.2, y, "•", w=0.6, fs=12, color=MUTED)
    for i in range(len(xs) - 1):
        if i == 3:
            continue
        arrow(ax, (xs[i] + 1.5, ys[i] + 0.5), (xs[i + 1], ys[i + 1] + 0.5), color=INK)
    # the splice: 17 -> 21 -> 26
    cell(ax, 10.5, 2.9, 21, w=1.2, edge=RED, fill=RED_FILL, color=RED)
    cell(ax, 11.7, 2.9, "•", w=0.6, fs=12, color=RED, edge=RED, fill=RED_FILL)
    arrow(ax, (xs[3] + 1.5, ys[3] + 0.5), (xs[4], ys[4] + 0.5), color=FAINT, ls="--")
    arrow(ax, (xs[3] + 1.5, ys[3] + 0.9), (10.5, 3.4), color=RED, lw=1.8)
    arrow(ax, (12.0, 2.9), (12.4, ys[4] + 1.0), color=RED, lw=1.8)
    label(ax, 13.3, 3.4, "INSERT(21): change two pointers.\nNothing moves.", color=RED, fs=9)
    label(ax, 0, -1.2, "But to find where 21 goes, or the 5th key, you start at the head and follow", fs=9)
    label(ax, 0, -1.9, "pointers one at a time. Each hop is a trip to memory you can't start early.", fs=9)
    label(ax, 0, -2.6, "Search and at() are O(n), and so is insert, once you count finding the spot.", fs=9)
    save(fig, "linked_list.png")


# ---- binary search tree: the best of both, on paper --------------------------------

TREE15 = [34, 17, 51, 8, 26, 42, 60, 3, 12, 21, 30, 39, 47, 55, 64]


def tree(ax, keys, x0, y0, width, dy, path=(), new=None, fs=10, r=0.42):
    """heap-ordered keys (None = empty) drawn as a tree; returns node centres"""
    pos = {}
    for i, k in enumerate(keys):
        if k is None:
            continue
        depth = (i + 1).bit_length() - 1
        slot = i - (2**depth - 1)
        x = x0 + width * (slot + 0.5) / 2**depth
        y = y0 - depth * dy
        pos[i] = (x, y)
    for i, (x, y) in pos.items():
        for c in (2 * i + 1, 2 * i + 2):
            if c in pos:
                hot = i in path and c in path
                ax.plot([x, pos[c][0]], [y, pos[c][1]], color=RED if hot else INK, linewidth=2 if hot else 1.2, zorder=2)
    for i, (x, y) in pos.items():
        hot, fresh = i in path, keys[i] == new
        ax.add_patch(plt.Circle((x, y), r, facecolor=PINK if (hot or fresh) else "white",
                                edgecolor=RED if (hot or fresh) else INK, linewidth=2 if hot else 1.4, zorder=3))
        ax.text(x, y, str(keys[i]), ha="center", va="center", fontsize=fs, color=RED if (hot or fresh) else INK, zorder=4)
    return pos


def binary_tree():
    fig, ax = canvas(10, 4.6, (-0.5, 17), (-1.6, 5.2))
    heading(ax, 0, 4.9, "BINARY SEARCH TREE: SMALLER KEYS LEFT, BIGGER KEYS RIGHT")
    path = (0, 2, 5)   # 34 -> 51 -> 42
    tree(ax, TREE15, 0, 3.9, 11, 1.2, path=path)
    label(ax, 11.3, 3.6, "SEARCH(42)\n34: go right\n51: go left\n42: found", color=RED, fs=9)
    label(ax, 11.3, 1.6, "Search: log₂ n steps, like the\nsorted array.\nInsert: hang a new node off\nthe bottom, like the list.\nNothing moves.", fs=9)
    label(ax, 0, -1.2, "at(i) is O(log n) too, if each node also keeps a count of the nodes under it.", fs=9)
    save(fig, "binary_tree.png")


def avl():
    fig, ax = canvas(10, 4.4, (-0.5, 18), (-1.4, 5.6))
    heading(ax, 0, 5.3, "INSERT 1..7 IN ORDER")
    label(ax, 0, 4.6, "plain binary tree: it's a linked list", fs=9)
    for d in range(7):
        x, y = 0.6 + d * 0.9, 3.8 - d * 0.75
        if d:
            ax.plot([x - 0.9, x], [y + 0.75, y], color=INK, linewidth=1.2, zorder=2)
        ax.add_patch(plt.Circle((x, y), 0.32, facecolor="white", edgecolor=INK, linewidth=1.4, zorder=3))
        ax.text(x, y, str(d + 1), ha="center", va="center", fontsize=9, zorder=4)
    label(ax, 10, 4.6, "AVL tree: rotations keep it balanced", fs=9, color=RED)
    tree(ax, [4, 2, 6, 1, 3, 5, 7], 9.5, 3.6, 7.5, 1.3, path=(), fs=10, r=0.38)
    label(ax, 10, -0.9, "Every node: key + 2 pointers + balance + count, its own allocation.", fs=9)
    label(ax, 0, -0.9, "7 deep for 7 keys", fs=9)
    save(fig, "avl.png")


# ---- diskarray: index in RAM, extents on disk -----------------------------------

def box(ax, x, y, w, h, title_, sub=None, edge=INK, fill="white", dashed=False, tcolor=INK):
    ax.add_patch(Rectangle((x, y), w, h, facecolor=fill, edgecolor=edge, linewidth=1.6, zorder=1,
                           linestyle="--" if dashed else "-"))
    ax.text(x + 0.2, y + h - 0.3, title_, ha="left", va="top", fontsize=10, fontweight="bold", color=tcolor, zorder=4)
    if sub:
        ax.text(x + 0.2, y + h - 0.75, sub, ha="left", va="top", fontsize=8, color=MUTED, zorder=4)


def diskarray():
    fig, ax = canvas(10, 6.0, (0, 20), (0, 12))
    # RAM: the index
    box(ax, 0.3, 6.4, 11.4, 5.3, "RAM: THE INDEX ONLY", "~1.5% of the data: 7.5 GiB for 64G nodes")
    cell(ax, 0.8, 9.3, "top: one entry per directory block", w=10.4, h=0.8, fs=8.5)
    for j in range(5):
        cell(ax, 0.8 + j * 2.1, 7.8, "dir block", w=1.9, h=0.8, fs=8, color=MUTED)
        arrow(ax, (1.75 + j * 2.1, 9.3), (1.75 + j * 2.1, 8.6), color=MUTED, lw=1)
    label(ax, 0.8, 7.1, "each dir block: up to 4096 extents' min, max, count", fs=8)
    # L3: buffer pool
    box(ax, 12.3, 6.4, 7.4, 5.3, "L3: 24 MiB BUFFER POOL", "6144 x 4 KiB, DDIO lands reads here", edge=RED,
        fill=RED_FILL, tcolor=RED)
    for j in range(12):
        cell(ax, 12.8 + (j % 6) * 1.1, 8.6 - (j // 6) * 1.1, "", w=0.9, h=0.9, edge=RED, lw=1)
    label(ax, 12.8, 6.9, "2048 transactions in flight", color=RED, fs=8.5)
    # disk: extents
    box(ax, 0.3, 0.3, 19.4, 5.2, "8 NVMe DRIVES: THE DATA", "every extent is one 4 KiB block: up to 512 sorted 8-byte keys, with slack")
    for d in range(8):
        x = 0.8 + d * 2.35
        for r in range(3):
            cell(ax, x, 1.0 + r * 1.0, "", w=2.0, h=0.8, lw=1, edge=MUTED)
            for t in range(4):
                ax.plot([x + 0.2 + t * 0.45, x + 0.45 + t * 0.45], [1.4 + r * 1.0] * 2, color=TRACE, linewidth=2, zorder=4)
    arrow(ax, (16.0, 6.3), (16.0, 4.2), color=RED, lw=2, style="<|-|>")
    label(ax, 15.7, 5.9, "io_uring, 28 workers\n4 KiB reads and writes", color=RED, fs=8.5, ha="right")
    label(ax, 0.5, 5.95, "index says which extent; the extent says where in it", fs=8.5)
    save(fig, "diskarray.png")


# ---- pipelining: one at a time vs a group resolved level by level ----------------

def disk_pipeline():
    """DiskArray's queue, schematic (not to scale): the core lane, and a few of the 6144 IO slots
    (4 KiB buffers) over three half-batch periods.  A slot's life spans batches: free -> read in
    flight (requested when its half is queued) -> in RAM, waiting for the core -> used by one insert,
    now dirty -> written back in the next flush (once 1536 are dirty), copy-on-write -> free again."""
    P, Q = 10.0, 1.0                     # one period: queue the next half, then run this half
    fig, ax = plt.subplots(figsize=(11, 5.2), constrained_layout=True)
    CORE, h = 6.4, 0.55
    core_kw = dict(facecolor=INK, edgecolor=GROUND, linewidth=1, zorder=3)
    queue_kw = dict(facecolor=MUTED, edgecolor=GROUND, linewidth=1, zorder=3)

    # core lane
    periods = [("B", "A", "queue half B"), ("A", "B", "queue half A"), ("B", "A", "queue half B\n+ flush dirty")]
    run_x = {}
    for k, (nxt, cur, qlbl) in enumerate(periods):
        t0 = k * P
        ax.add_patch(Rectangle((t0, CORE), Q, h, **queue_kw))
        ax.text(t0 + Q / 2, CORE + h + 0.1, qlbl, ha="center", va="bottom", fontsize=7, color=MUTED, linespacing=1.0)
        x, w = t0 + Q + 0.15, 1.15
        cur = cur + ("'" if k == 2 else "")
        for name in (f"{cur}1", f"{cur}2", f"{cur}3", "...", f"{cur}1024"):
            if name == "...":
                ax.text(x + 1.2, CORE + h / 2, "...", ha="center", va="center", fontsize=10, color=MUTED)
                x += 2.4
                continue
            ax.add_patch(Rectangle((x, CORE), w, h, **core_kw))
            ax.text(x + w / 2, CORE + h / 2, name, ha="center", va="center", fontsize=7, color="white", zorder=4)
            run_x[name] = x + w / 2
            x += w + 0.15

    # IO slots: (label, read issued, read done, used at, write issued, write done)
    rows = [
        ("B1", 0.2, 3.2, run_x["B1"], 20.3, 24.0),
        ("B2", 0.2, 4.7, run_x["B2"], 20.3, 25.0),
        ("B3", 0.4, 3.9, run_x["B3"], 20.4, 23.5),
        ("B1024", 0.9, 5.9, run_x["B1024"], 20.6, 26.0),
        ("A'1", 10.2, 14.2, run_x["A'1"], None, None),
        ("A'2", 10.2, 13.2, run_x["A'2"], None, None),
        ("A'1024", 10.9, 14.4, run_x["A'1024"], None, None),
    ]
    END = 31.5
    slot_y = [4.9 - i * 0.62 for i in range(len(rows) + 1)]
    bh = 0.42

    def seg(y, a, b, kind, text=None):
        if kind == "free":
            kw = dict(facecolor="none", edgecolor=FAINT, linewidth=1, linestyle="--")
        elif kind == "read":
            kw = dict(facecolor=PINK, edgecolor=RED, linewidth=1)
        elif kind == "ram":
            kw = dict(facecolor="white", edgecolor=RED, linewidth=0.8)
        elif kind == "dirty":
            kw = dict(facecolor=TRACE, edgecolor=MUTED, linewidth=0.8)
        else:  # write
            kw = dict(facecolor="white", edgecolor=INK, linewidth=1, hatch="///")
        ax.add_patch(Rectangle((a, y), b - a, bh, zorder=3, **kw))
        if text:
            ax.text((a + b) / 2, y + bh / 2, text, ha="center", va="center", fontsize=6.5, zorder=4,
                    color=RED if kind in ("read", "ram") else INK,
                    bbox=dict(facecolor=GROUND if kind == "write" else "none", edgecolor="none", pad=0.3))

    for i, (name, r0, r1, used, w0, w1) in enumerate(rows):
        y = slot_y[i if i < 4 else i + 1]
        first = i in (0, 4)
        seg(y, -0.6 if i < 4 else -0.6, r0, "free", "free" if first else None)
        seg(y, r0, r1, "read", "read" if first or i == 3 else None)
        seg(y, r1, used, "ram", "in RAM, waiting for the core" if i == 0 else ("in RAM" if first else None))
        ax.add_patch(Rectangle((used - 0.08, y - 0.06), 0.16, bh + 0.12, facecolor=INK, edgecolor=INK, zorder=5))
        ax.plot([used, used], [y + bh + 0.06, CORE], color=TRACE, linewidth=0.8, linestyle=":", zorder=1)
        if w0:
            seg(y, used + 0.08, w0, "dirty", "dirty" if first else None)
            seg(y, w0, w1, "write", "write" if first else None)
            seg(y, w1, END, "free", "free again, ready for a new read" if first else None)
        else:
            seg(y, used + 0.08, END, "dirty", "dirty, waits for the next flush" if first else None)
        ax.text(-0.9, y + bh / 2, name, ha="right", va="center", fontsize=7.5, color=MUTED)
    ax.text(-0.9, slot_y[4] + bh / 2, "...", ha="right", va="center", fontsize=10, color=MUTED)

    label(ax, -3.2, CORE + h / 2, "CORE", color=INK, fs=10, ha="right", weight="bold")
    label(ax, -3.2, (slot_y[0] + slot_y[-1]) / 2 + bh / 2, "IO SLOTS", color=RED, fs=10, ha="right", weight="bold")
    ax.text(-3.2, (slot_y[0] + slot_y[-1]) / 2 - 0.3, "6144 x 4 KiB", color=MUTED, fontsize=7.5, ha="right")
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(facecolor="none", edgecolor=FAINT, linestyle="--", label="free"),
                       Patch(facecolor=PINK, edgecolor=RED, label="read in flight"),
                       Patch(facecolor="white", edgecolor=RED, label="in RAM, waiting"),
                       Patch(facecolor=INK, edgecolor=INK, label="used by an insert"),
                       Patch(facecolor=TRACE, edgecolor=MUTED, label="dirty"),
                       Patch(facecolor="white", edgecolor=INK, hatch="///", label="write in flight")],
              loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=6, fontsize=8, frameon=False)
    ax.set_xlim(-6.2, END + 0.3)
    ax.set_ylim(slot_y[-1] - 0.9, CORE + 1.6)
    ax.axis("off")
    ax.set_title("DISKARRAY: AN IO SLOT'S LIFE SPANS BATCHES", loc="left", fontweight="bold", fontsize=12, color=INK)
    ax.text(-6.2, CORE + 1.45, "Schematic, not to scale. 2048 transactions in flight as two halves of 1024; "
            "a few of the 6144 buffer slots shown.", fontsize=8, color=MUTED, va="top")
    save(fig, "disk_pipeline.png")


def pipeline():
    """Illustrative timeline: one at a time vs a pipelined group, on one time axis.
    80 ns per DRAM miss, 3 index levels, 5 ns of core work per level, 20 ns to execute
    (the memmove, already prefetched), 2 ns to issue a prefetch, groups of 8."""
    MISS, WORK, EXEC, ISSUE, LEVELS, G = 80, 5, 20, 2, 3, 8
    fig, (top, bot) = plt.subplots(2, 1, figsize=(10, 5.6), sharex=True, constrained_layout=True,
                                   gridspec_kw=dict(height_ratios=[1.3, 4]))
    wait_kw = dict(height=0.6, facecolor=PINK, edgecolor=RED, linewidth=0.8, zorder=2)
    work_kw = dict(height=0.6, facecolor=INK, edgecolor=INK, linewidth=0, zorder=3)

    # one at a time: miss, work, miss, work, miss, work, execute; next transaction
    t, done_at = 0, []
    while t < 900:
        for _ in range(LEVELS):
            top.barh(0, MISS, left=t, **wait_kw)
            t += MISS
            top.barh(0, WORK, left=t, **work_kw)
            t += WORK
        top.barh(0, EXEC, left=t, **work_kw)
        t += EXEC
        done_at.append(t)
    for i, d in enumerate(done_at[:3]):
        top.text(d - (LEVELS * (MISS + WORK) + EXEC) / 2, 0.45, f"T{i + 1}", ha="center", va="bottom", fontsize=9, color=MUTED)

    # pipelined: each level, issue all G prefetches back to back, then handle each answer as it lands
    issue = [i * ISSUE for i in range(G)]
    core = 0
    for lvl in range(LEVELS):
        for i in range(G):
            bot.barh(G - 1 - i, MISS, left=issue[i], **wait_kw)
        nxt = []
        for i in range(G):
            core = max(core, issue[i] + MISS)
            bot.barh(G - 1 - i, WORK, left=core, **work_kw)
            core += WORK
            nxt.append(core)                          # issue this transaction's next level right away
        issue = nxt
    for i in range(G):                                # execute in order; the memmoves were prefetched
        bot.barh(G - 1 - i, EXEC, left=core, **work_kw)
        core += EXEC
    finish = core
    for i in range(G):
        bot.text(-8, G - 1 - i, f"T{i + 1}", ha="right", va="center", fontsize=9, color=MUTED)

    # the payoff, on the same clock
    one_done = sum(d <= finish for d in done_at)
    for ax in (top, bot):
        ax.axvline(finish, color=RED, linestyle="--", linewidth=1.4, zorder=4)
    top.text(finish + 6, 0.85, f"{one_done} done", va="center", fontsize=11, color=INK, fontweight="bold", zorder=5)
    bot.text(finish + 6, G / 2 - 0.5, f"{G} done", va="center", fontsize=11, color=RED, fontweight="bold")

    top.set_title("ONE AT A TIME: THE CORE MOSTLY WAITS", loc="left", fontweight="bold", fontsize=12, color=INK)
    bot.set_title(f"PIPELINED: ASK FOR LEVEL 1 OF ALL {G}, THEN LEVEL 2, THEN LEVEL 3; THE WAITS OVERLAP",
                  loc="left", fontweight="bold", fontsize=12, color=INK)
    for ax in (top, bot):
        ax.set_yticks([])
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.set_facecolor(GROUND)
    top.set_ylim(-0.6, 1.3)
    bot.set_ylim(-0.7, G - 0.3)
    bot.set_xlim(-40, 900)
    bot.set_xlabel("TIME (ns)  ·  illustrative: 80 ns per DRAM miss, 3 index levels, groups of 8 (the real code uses 32)\n"
                   "Real cores overlap ~12 misses and do more work per level: measured gain at 2G nodes is ~3x on search")
    from matplotlib.patches import Patch
    bot.legend(handles=[Patch(facecolor=PINK, edgecolor=RED, label="waiting on DRAM"),
                        Patch(facecolor=INK, edgecolor=INK, label="core working")],
               loc="lower right", fontsize=9, frameon=False)
    save(fig, "pipeline.png")


if __name__ == "__main__":
    setup()
    sorted_search()
    sorted_at()
    sorted_insert()
    sorted_delete()
    linked_list()
    binary_tree()
    avl()
    diskarray()
    disk_pipeline()
    pipeline()
