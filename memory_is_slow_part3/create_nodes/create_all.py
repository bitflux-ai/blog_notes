#!/usr/bin/env python3
import argparse
import subprocess
import shlex
import sys

# Generates sizes doubling from 16 KiB up to and including 1 GiB.
def generate_sizes():
    size_bytes = 16 * 1024  # 16 KiB
    end_bytes = 1 * 1024 ** 3  # 1 GiB
    while size_bytes <= end_bytes:
        yield size_bytes
        size_bytes *= 2

def fmt_size(b):
    # Prefer G, then M, else K (binary multiples: KiB/MiB/GiB)
    if b % (1024 ** 3) == 0:
        return f"{b // (1024 ** 3)}G"
    elif b % (1024 ** 2) == 0:
        return f"{b // (1024 ** 2)}M"
    else:
        return f"{b // 1024}K"

def main():
    parser = argparse.ArgumentParser(description="Run create_nodes with doubling -c sizes from 16K to 1G.")
    parser.add_argument("input", help="Input filename for -i")
    parser.add_argument("--cmd", default="./create_nodes/create_nodes", help="Path to create_nodes executable (default: ./create_nodes/create_nodes)")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing them")
    args = parser.parse_args()

    for b in generate_sizes():
        c_arg = fmt_size(b)
        cmd = [args.cmd, "-i", args.input, "-s", "-c", c_arg]
        print("Running:", " ".join(shlex.quote(x) for x in cmd))
        if not args.dry_run:
            try:
                subprocess.run(cmd, check=True)
            except subprocess.CalledProcessError as e:
                print(f"Command failed with exit code {e.returncode}", file=sys.stderr)
                sys.exit(e.returncode)

if __name__ == "__main__":
    main()
