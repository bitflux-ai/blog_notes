"""Shared look and data for the Part 3 charts and diagrams: the palette, the fonts in
fonts/, and the sweep table in ../run/results/unified/summary.txt."""
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager as fm, patheffects, rcParams

HERE = Path(__file__).resolve().parent
MEDIA = HERE / "fonts"
SUMMARY = HERE.parent / "run" / "results" / "unified" / "summary.txt"

INK, BODY, MUTED, FAINT, TRACE, RED, GROUND = "#1c1b18", "#4a463c", "#7a7568", "#9a958a", "#dcd6c6", "#e01010", "#f5f4f0"
RED_FILL = (0.88, 0.06, 0.06, 0.15)
DPI, SCALE = 100, 2
OPS = ("search", "index", "insert", "delete")
OP_LABEL = {"search": "SEARCH", "index": "AT(i)", "insert": "INSERT", "delete": "DELETE"}
TPS = "TRANSACTIONS / S"

# AVL: libavl node (5 pointers, item, count, depth) is a 64 B allocation, plus the 8 B key it
# points at: 72 B per node, which matches peak RSS (144.45 GiB at 2G nodes)
AVL_NODE_BYTES = 72
L3_BYTES = 35.75 * 2**20
AVL_L3_NODES = 2**19      # 512K x 72 B = 36 MiB, the last doubling that (just about) fits

# diskarray's peak RSS in summary.txt is sudo's; its own "RAM usage" line, from the 64G-node run
DISK_RAM_GIB = 7.52


def load():
    """sweep section of summary.txt -> {structure: {"nodes": [], "rss": [], <op>: [ns or None]}}"""
    text = SUMMARY.read_text().split("sweep (ns/op)", 1)[1]
    out, cur = {}, None
    for line in text.splitlines():
        if m := re.match(r"^(\w+)(?: \(extent \d+\))?$", line):
            cur = out.setdefault(m[1], {"nodes": [], "rss": [], **{p: [] for p in OPS}})
        elif cur is not None and (m := re.match(r"^\s+(\d+)\s+(.*)$", line)) and "+" not in line:
            # search index insert delete [clustered] [sum] rss; n/a for skipped
            vals = [None if c == "n/a" else float(c) for c in m[2].split()]
            cur["nodes"].append(int(m[1]))
            for p, v in zip(OPS, vals):
                cur[p].append(v)
            cur["rss"].append(vals[-1])
        elif line.startswith("  drive traffic"):
            cur = None
    return out


D = load()


def ns(s, op, nodes):
    return D[s][op][D[s]["nodes"].index(nodes)]


def tps(s, op, nodes):
    return 1e9 / ns(s, op, nodes)


def series(s, op, max_nodes=None, min_nodes=None):
    """nodes[], transactions/s[]; NaN where skipped or the ns/op rounds to 0"""
    n = np.array(D[s]["nodes"], float)
    y = np.array([np.nan if not v else 1e9 / v for v in D[s][op]], float)
    keep = (n <= (max_nodes or np.inf)) & (n >= (min_nodes or 0))
    n, y = n[keep], y[keep]
    return n, y


def setup():
    for ttf in sorted(MEDIA.glob("*.ttf")):
        fm.fontManager.addfont(str(ttf))
    rcParams.update({
        "font.family": "Chakra Petch", "font.weight": "medium",
        "text.color": INK, "axes.labelcolor": MUTED, "axes.edgecolor": INK,
        "xtick.color": MUTED, "ytick.color": MUTED,
        "axes.titlesize": 14, "axes.labelsize": 10, "xtick.labelsize": 10, "ytick.labelsize": 10,
        "legend.fontsize": 9, "legend.frameon": False, "axes.unicode_minus": False,
        "figure.facecolor": GROUND, "savefig.facecolor": GROUND,
    })


def frame(ax, grid_axis="y"):
    ax.set_facecolor(GROUND)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_linewidth(1.2)
    ax.grid(True, axis=grid_axis, color=TRACE, linewidth=1)
    ax.set_axisbelow(True)
    ax.tick_params(length=4, width=1, colors=MUTED)


def title(ax, text, sub=None):
    ax.set_title(text, loc="left", color=INK, pad=22 if sub else 10, fontweight="bold")
    if sub:
        ax.text(0, 1.02, sub, transform=ax.transAxes, color=MUTED, fontsize=9, va="bottom")


def save(fig, name):
    out = HERE / name
    fig.savefig(out, dpi=DPI * SCALE)
    plt.close(fig)
    print(out.name)


def count(v):
    """2048 -> 2K, 2**30 -> 1G"""
    for div, suf in ((2**30, "G"), (2**20, "M"), (2**10, "K")):
        if v >= div:
            return f"{v / div:.0f}{suf}"
    return f"{v:.0f}"


def rate(v):
    for div, suf in ((1e9, "G"), (1e6, "M"), (1e3, "K")):
        if v >= div:
            return f"{v / div:.3g}{suf}"
    return f"{v:.3g}"


def nodes_axis(ax, lo=11, hi=36, step=5):
    ax.set_xscale("log", base=2)
    ticks = [2**k for k in range(lo, hi + 1, step)]
    ax.set_xticks(ticks, [count(t) for t in ticks])
    ax.minorticks_off()
    ax.set_xlabel("NODES")


def tps_axis(ax, subs=(1.0,)):
    """log axis labelled 1K/10K/...; subs=(1, 2, 5) adds 2s and 5s for narrow ranges"""
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(plt.LogLocator(base=10, subs=subs))
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: rate(v)))
    ax.yaxis.set_minor_formatter(plt.NullFormatter())


def vline(ax, x, text, color=MUTED, style=":"):
    ax.axvline(x, color=color, linestyle=style, linewidth=1.2, zorder=2)
    ax.text(x, 0.99, f" {text}", transform=ax.get_xaxis_transform(), color=color, fontsize=8, ha="left", va="top")


def nixie_ax(ax, x, y, text, size=40, label=None):
    """A red Nixie readout with a soft glow, right-aligned at (x, y) in data coordinates (as Part 2.5's nixie)."""
    for lw, alpha in ((10, 0.04), (5, 0.07)):
        ax.text(x, y, text, fontfamily="Nixie One", fontsize=size, color=RED, alpha=alpha, ha="right", va="center",
                path_effects=[patheffects.withStroke(linewidth=lw, foreground=RED, alpha=alpha)], zorder=6)
    ax.text(x, y, text, fontfamily="Nixie One", fontsize=size, color="#ff2a2a", ha="right", va="center", zorder=7)
    if label:
        ax.text(x, y - 0.42, label, fontsize=8, color=MUTED, ha="right", va="top", zorder=7)
