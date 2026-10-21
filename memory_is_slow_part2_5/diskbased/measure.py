#!/usr/bin/env python3
"""Run ./benchmark under DRAM and PCIe counters and write one JSON report.

Rates are over the benchmark's timed section (its printed seconds).  perf's
own elapsed time also includes setup, so the counter totals are an upper
bound on what the timed section moved.

    sudo ./measure.py -o run.json -- -t targets.yml -w 1 -q 8 103079215104
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

GIB = 1024 ** 3
IIO_STACKS = 6
IIO_PORTS = 4


def ddio_llc_ways():
    """Bitmask of L3 ways DDIO may allocate into (MSR 0xC8B, IIO_LLC_WAYS).
    0x600 is the 2-way default; 0x7FF opens all 11 ways on an 11-way L3."""
    try:
        with open("/dev/cpu/0/msr", "rb") as f:
            f.seek(0xC8B)
            mask = int.from_bytes(f.read(8), "little") & 0x7FF
    except OSError:
        return None
    return {"mask": hex(mask), "ways": bin(mask).count("1")}


def pcie_events():
    ev = ["uncore_imc/cas_count_read/", "uncore_imc/cas_count_write/"]
    for stack in range(IIO_STACKS):
        for port in range(IIO_PORTS):
            ev.append(f"uncore_iio_free_running_{stack}/bw_in_port{port}/")
            ev.append(f"uncore_iio_free_running_{stack}/bw_out_port{port}/")
    return ev


def parse_benchmark(text):
    head = re.search(
        r"(\d+) drive\(s\), (\d+) workers, depth (\d+), (\d+)-byte blocks, "
        r"([0-9.]+) MiB pool \(L3 ([0-9.]+) MiB\), count (\S+)",
        text,
    )
    if not head:
        raise SystemExit("benchmark output had no summary line")
    drives = []
    for m in re.finditer(r"^\s*\[(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9.]+) GiB", text, re.M):
        drives.append({
            "index": int(m.group(1)),
            "path": m.group(2),
            "nqn": m.group(3),
            "gib": float(m.group(4)),
        })
    aff = re.search(r"affinity: main cpu (\d+), sibling (-?\d+) idle, workers([0-9 ]*)", text)
    result = re.search(
        r"result: count=(\S+) bytes=(\d+) seconds=([0-9.]+) GiB/s=([0-9.]+) busy=([0-9.]+) busyGiB/s=([0-9.]+) tens=(\d+)", text
    )
    if not result:
        raise SystemExit("benchmark output had no result line")
    out = {
        "drives_n": int(head.group(1)),
        "workers": int(head.group(2)),
        "depth": int(head.group(3)),
        "block_bytes": int(head.group(4)),
        "pool_mib": float(head.group(5)),
        "l3_mib": float(head.group(6)),
        "count_function": head.group(7),
        "targets": drives,
        "bytes": int(result.group(2)),
        "payload_gib": int(result.group(2)) / GIB,
        "timed_s": float(result.group(3)),
        "gib_s": float(result.group(4)),
        "consumer_busy": float(result.group(5)),
        "consumer_busy_gib_s": float(result.group(6)),
        "tens": int(result.group(7)),
    }
    if aff:
        out["affinity"] = {
            "main_cpu": int(aff.group(1)),
            "sibling_idle": int(aff.group(2)),
            "workers": [int(x) for x in aff.group(3).split()],
        }
    return out


def parse_perf(text):
    counters = {}
    for m in re.finditer(r"^\s*([0-9.]+)\s+MiB\s+(\S+)\s*$", text, re.M):
        counters[m.group(2)] = float(m.group(1))
    elapsed = re.search(r"([0-9.]+) seconds time elapsed", text)
    return counters, float(elapsed.group(1)) if elapsed else None


def per_s(mib_val, timed_s):
    return round(mib_val / 1024.0 / timed_s, 4) if timed_s else None


def main():
    here = Path(__file__).resolve().parent
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("-o", "--output", help="JSON file (default: stdout)")
    p.add_argument("--benchmark", default=str(here / "benchmark"))
    args, bench_args = p.parse_known_args()
    if bench_args[:1] == ["--"]:
        bench_args = bench_args[1:]
    if not bench_args:
        p.error("pass benchmark arguments, e.g. -t targets.yml -w 1 -q 8 103079215104")
    if os.geteuid() != 0:
        sys.exit("run with sudo so perf can read uncore counters and the drives")

    cmd = ["perf", "stat", "-a", "-e", ",".join(pcie_events()), "--", args.benchmark, *bench_args]
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        sys.exit(proc.returncode)

    bench = parse_benchmark(proc.stdout)
    counters, perf_s = parse_perf(proc.stderr)
    timed = bench["timed_s"]
    ports = []
    pcie_in = pcie_out = 0.0
    for stack in range(IIO_STACKS):
        for port in range(IIO_PORTS):
            inn = counters.get(f"uncore_iio_free_running_{stack}/bw_in_port{port}/", 0.0)
            out = counters.get(f"uncore_iio_free_running_{stack}/bw_out_port{port}/", 0.0)
            pcie_in += inn
            pcie_out += out
            if inn >= 1.0 or out >= 1.0:
                ports.append({
                    "stack": stack,
                    "port": port,
                    "in_mib": round(inn, 2),
                    "out_mib": round(out, 2),
                    "in_gib_s": per_s(inn, timed),
                    "out_gib_s": per_s(out, timed),
                })
    dram_r = counters.get("uncore_imc/cas_count_read/", 0.0)
    dram_w = counters.get("uncore_imc/cas_count_write/", 0.0)
    report = {
        "command": [args.benchmark, *bench_args],
        "benchmark": bench,
        "ddio_llc_ways": ddio_llc_ways(),
        "dram": {
            "read_mib": round(dram_r, 2),
            "write_mib": round(dram_w, 2),
            "read_gib_s": per_s(dram_r, timed),
            "write_gib_s": per_s(dram_w, timed),
            "read_fraction_of_payload": round(dram_r * 1024 ** 2 / bench["bytes"], 4) if bench["bytes"] else None,
        },
        "pcie": {
            "in_mib": round(pcie_in, 2),
            "out_mib": round(pcie_out, 2),
            "in_gib_s": per_s(pcie_in, timed),
            "out_gib_s": per_s(pcie_out, timed),
            "ports": ports,
        },
        "perf_elapsed_s": perf_s,
        "rate_window": "benchmark timed section; perf_elapsed_s also includes setup",
    }
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text)
        print(args.output, file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
