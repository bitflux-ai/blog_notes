#!/usr/bin/env python3
import argparse
import subprocess
import shlex
import time
import os
import sys

# Generates sizes doubling from 16 KiB up to and including 1 GiB.
def generate_sizes():
    size_bytes = 16 * 1024  # 16 KiB
    end_bytes = 8 * 1024 ** 3  # 8 GiB
    while size_bytes <= end_bytes:
        yield size_bytes
        size_bytes *= 2

def _fmt_three_sig(n: int) -> str:
    """Format positive integer n with 3 significant digits:
       1   -> 1.00
       16  -> 16.0
       128 -> 128
    """
    if n >= 100:
        return f"{n:d}"
    elif n >= 10:
        return f"{n:.1f}"
    else:
        return f"{n:.2f}"

def fmt_size(b: int) -> str:
    """Format bytes as K/M/G with 3 significant digits in the mantissa
    using binary multiples (KiB/MiB/GiB) with the 'i' in the suffix.
    Examples: 16.0KiB, 8.00MiB, 128KiB, 1.00GiB.
    """
    if b % (1024 ** 3) == 0:
        val = b // (1024 ** 3)
        return f"{_fmt_three_sig(int(val))}GiB"
    elif b % (1024 ** 2) == 0:
        val = b // (1024 ** 2)
        return f"{_fmt_three_sig(int(val))}MiB"
    else:
        val = b // 1024
        return f"{_fmt_three_sig(int(val))}KiB"

# ---- Extent-size sweep helpers ----
def generate_extents(max_for_b_bytes: int):
    """Yield extent sizes (in bytes) doubling from 8 up to min(128KiB, max_for_b_bytes).
    The caller passes max_for_b_bytes = b // 8.
    """
    limit = min(128 * 1024, max_for_b_bytes)  # cap at 128 KiB or b/8, whichever is smaller
    v = 8  # start at 8 bytes
    while v <= limit:
        yield v
        v *= 2

def fmt_extent(v_bytes: int) -> str:
    """Format extent as: 8 16 32 ... 512 1K 2K ... 1M (no decimals, no 'i')."""
    if v_bytes < 1024:
        return str(v_bytes)
    elif v_bytes < 1024 * 1024:
        return f"{v_bytes // 1024}K"
    else:
        return f"{v_bytes // (1024 * 1024)}M"

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Run ./benchmark with --input and --list for sizes from 16KiB .. 1GiB.\n"
            "For each size, also sweep --extent-size doubling from 8 .. 128KiB, but never exceeding 1/8 of that size.\n"
            "Measure elapsed time per run; if elapsed > --timeout, stop all further runs (no hard timeout)."
        )
    )
    parser.add_argument("-t", "--timeout", type=float, required=True,
                        help="Threshold in seconds to decide whether to continue to the next run")
    parser.add_argument("-e", "--extent-size", type=int, help="override extent size")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing them")
    args = parser.parse_args()

    stop_all = False
    for b in generate_sizes():
        size_str = fmt_size(b)
        # extent must be <= b/8 (bytes)
        extent_list = []
        if args.extent_size is not None:
            extent_list = [args.extent_size]
        else:
            max_extent_for_b = b // 8
            extent_list = list(generate_extents(max_extent_for_b))

        for e in extent_list:
            extent_str = fmt_extent(e)
            cmd = [
                "./benchmark",
                "--input", "../data/nodes-F37FB6DD-11.8GiB.bin",
                "--list", f"../data/nodes-sorted-F37FB6DD-{size_str}-original.bin",
                "--extent-size", extent_str,
            ]

            print("Running:", " ".join(shlex.quote(x) for x in cmd))
            if args.dry_run:
                continue

            start = time.monotonic()
            try:
                # Let the process finish naturally; we only *measure* elapsed
                result = subprocess.run(cmd, check=False)
                duration = time.monotonic() - start
                print(f"Finished size={size_str} extent={extent_str} exit={result.returncode} elapsed={duration:.3f}s")
            except Exception as e:
                duration = time.monotonic() - start
                print(f"Error after {duration:.3f}s at size={size_str} extent={extent_str}: {e}", file=sys.stderr)
                # Continue unless the elapsed threshold is exceeded
                # (we treat exceptions as non-fatal for scheduling purposes).
                # You can change this behavior if desired.
                pass

            # Decide whether to proceed to the next run based on elapsed time
            if args.timeout > 0 and duration > args.timeout:
                print(f"Elapsed {duration:.3f}s exceeded threshold {args.timeout:.3f}s at size={size_str} extent={extent_str}. Stopping further runs.")
                stop_all = True
                break

        if stop_all:
            break

if __name__ == "__main__":
    main()
