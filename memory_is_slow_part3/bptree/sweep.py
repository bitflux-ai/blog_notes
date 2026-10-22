#!/usr/bin/env python3
"""Size ladder (+ optional leaf-size sweep) for the B+ tree benchmark.

Mirrors nextarray/sweep.py so supersweep can treat leaf_size like extent_size.
Default leaf-size is 512 (fair peer to nextarray extent_size=512).
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
import time


def generate_sizes():
    size_bytes = 16 * 1024  # 16 KiB
    end_bytes = 8 * 1024 ** 3  # 8 GiB
    while size_bytes <= end_bytes:
        yield size_bytes
        size_bytes *= 2


def _fmt_three_sig(n: int) -> str:
    if n >= 100:
        return f"{n:d}"
    elif n >= 10:
        return f"{n:.1f}"
    else:
        return f"{n:.2f}"


def fmt_size(b: int) -> str:
    if b % (1024 ** 3) == 0:
        val = b // (1024 ** 3)
        return f"{_fmt_three_sig(int(val))}GiB"
    elif b % (1024 ** 2) == 0:
        val = b // (1024 ** 2)
        return f"{_fmt_three_sig(int(val))}MiB"
    else:
        val = b // 1024
        return f"{_fmt_three_sig(int(val))}KiB"


def generate_leaf_sizes(max_for_b_bytes: int):
    """Yield leaf sizes in *keys* (nodes), doubling from 2 up to min(16384, max keys)."""
    # nextarray sweeps extent *bytes*; here leaf-size is node count.
    # Cap at 16K keys (~128 KiB of uint64) similar spirit to nextarray's 128KiB cap.
    limit = min(16384, max(2, max_for_b_bytes // 8))
    v = 2
    while v <= limit:
        yield v
        v *= 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run ./benchmark with --input and --list for sizes from 16KiB .. 8GiB.\n"
            "Optionally sweep --leaf-size (or pin with -e). Measure elapsed; if elapsed\n"
            "> --timeout, stop further runs."
        )
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        required=True,
        help="Threshold in seconds to decide whether to continue to the next run",
    )
    parser.add_argument(
        "-e",
        "--leaf-size",
        type=int,
        default=None,
        help="Pin leaf size (keys per leaf). Default: sweep, or 512 if --no-leaf-sweep",
    )
    parser.add_argument(
        "--no-leaf-sweep",
        action="store_true",
        help="Use a single leaf size (512 unless -e given) instead of a leaf-size ladder",
    )
    parser.add_argument(
        "-b",
        "--branch-factor",
        type=int,
        default=16,
        help="Internal max children (default 16)",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print commands only")
    args = parser.parse_args()

    stop_all = False
    for b in generate_sizes():
        size_str = fmt_size(b)
        if args.leaf_size is not None:
            leaf_list = [args.leaf_size]
        elif args.no_leaf_sweep:
            leaf_list = [512]
        else:
            leaf_list = list(generate_leaf_sizes(b))

        for leaf in leaf_list:
            cmd = [
                "./benchmark",
                "--input",
                "../data/nodes-F37FB6DD-11.8GiB.bin",
                "--list",
                f"../data/nodes-sorted-F37FB6DD-{size_str}-original.bin",
                "--leaf-size",
                str(leaf),
                "--branch-factor",
                str(args.branch_factor),
            ]
            print("Running:", " ".join(shlex.quote(x) for x in cmd))
            if args.dry_run:
                continue

            start = time.monotonic()
            duration = 0.0
            try:
                result = subprocess.run(cmd, check=False)
                duration = time.monotonic() - start
                print(
                    f"Finished size={size_str} leaf={leaf} "
                    f"exit={result.returncode} elapsed={duration:.3f}s"
                )
            except Exception as exc:
                duration = time.monotonic() - start
                print(
                    f"Error after {duration:.3f}s at size={size_str} leaf={leaf}: {exc}",
                    file=sys.stderr,
                )

            if args.timeout > 0 and duration > args.timeout:
                print(
                    f"Elapsed {duration:.3f}s exceeded threshold {args.timeout:.3f}s "
                    f"at size={size_str} leaf={leaf}. Stopping further runs."
                )
                stop_all = True
                break

        if stop_all:
            break

    return 0


if __name__ == "__main__":
    sys.exit(main())
