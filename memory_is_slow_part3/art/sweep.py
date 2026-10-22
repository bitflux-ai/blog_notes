#!/usr/bin/env python3
"""Size ladder for the ART benchmark (same shape as avl/sweep.py)."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
import time


def generate_sizes():
    size_bytes = 16 * 1024
    end_bytes = 8 * 1024 ** 3
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run ./benchmark for sizes 16KiB .. 8GiB. Stop further sizes when a "
            "completed run's elapsed time exceeds -t/--timeout."
        )
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        required=True,
        help="Elapsed-time threshold in seconds",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print commands only")
    args = parser.parse_args()

    for b in generate_sizes():
        size = fmt_size(b)
        cmd = [
            "./benchmark",
            "--input",
            "../data/nodes-F37FB6DD-11.8GiB.bin",
            "--list",
            f"../data/nodes-sorted-F37FB6DD-{size}-original.bin",
        ]
        print("Running:", " ".join(shlex.quote(x) for x in cmd))
        if args.dry_run:
            continue

        start = time.monotonic()
        duration = 0.0
        try:
            result = subprocess.run(cmd, check=False)
            duration = time.monotonic() - start
            print(f"Finished count={size} exit={result.returncode} elapsed={duration:.3f}s")
        except Exception as exc:
            duration = time.monotonic() - start
            print(f"Error after {duration:.3f}s at count={size}: {exc}", file=sys.stderr)
            continue

        if duration > args.timeout and args.timeout > 0:
            print(
                f"Elapsed {duration:.3f}s exceeded timeout threshold "
                f"{args.timeout:.3f}s at count={size}. Stopping further runs."
            )
            break

    return 0


if __name__ == "__main__":
    sys.exit(main())
