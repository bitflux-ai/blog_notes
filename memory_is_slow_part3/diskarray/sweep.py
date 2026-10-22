#!/usr/bin/env python3
import argparse
import subprocess
import shlex
import time
import os
import sys
from shutil import which

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
    using binary multiples (KiB/MiB/GiB) but without the 'i' in the suffix.
    Examples: 16.0K, 8.00M, 128K, 1.00G.
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

def resolve_executable(cmd):
    # If user passed a bare command, resolve via PATH; otherwise allow direct path.
    resolved = which(cmd)
    if resolved:
        return resolved
    if os.path.exists(cmd):
        return cmd
    return None

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Run FILE -l <count> for doubling sizes from 16K up to 1G.\n"
            "Measure elapsed time for each run; if elapsed > --timeout, stop scheduling further runs."
        )
    )
    parser.add_argument("-t", "--timeout", type=float, required=True, help="Threshold in seconds to decide whether to continue to next run")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing them")
    args = parser.parse_args()

    "sudo ./batched_bench -i ../data/nodes-984594C8-4.00GiB.bin -l ../data/nodes-sorted-984594C8-1.00GiB-original.bin --io-buffers 16384 --batch-size 2048 -f /dev/md0 -e 512 -d1 --workers 32"

    for b in generate_sizes():
        size = fmt_size(b)
        cmd = ["./batched_bench",
               "--input", "../data/nodes-F37FB6DD-11.8GiB.bin",
               "--list", f"../data/nodes-sorted-F37FB6DD-{size}-original.bin",
               "--io-buffers", "6144",
               "--batch-size", "2048",
               "-t", "../memory_is_slow_part2/diskbased/targets.yml",
               "-e", "512",
               "--workers", "28",
            ]
        print("Running:", " ".join(shlex.quote(x) for x in cmd))
        if args.dry_run:
            continue

        start = time.monotonic()
        try:
            # No enforced timeout: let the process finish naturally
            result = subprocess.run(cmd, check=False)
            duration = time.monotonic() - start
            print(f"Finished count={size} exit={result.returncode} elapsed={duration:.3f}s")
        except Exception as e:
            duration = time.monotonic() - start
            print(f"Error after {duration:.3f}s at count={size}: {e}", file=sys.stderr)
            # Keep behavior simple: only stop due to elapsed > timeout; otherwise continue.
            continue

        # Decide whether to proceed to the next run based on elapsed time
        if duration > args.timeout and args.timeout > 0:
            print(f"Elapsed {duration:.3f}s exceeded timeout threshold {args.timeout:.3f}s at count={size}. Stopping further runs.")
            break
        time.sleep(5)

if __name__ == "__main__":
    main()
