#!/usr/bin/env python3
"""Summarize a part3 results folder: results/<ts>/runs/<phase>/<name>-<size>[-e<E>]/
-> results.json (one row per run and operation) and the summary table on stdout.

    summarize.py results/<ts> <generated node file>

run_part3.sh calls this at the end of every run; merge_runs.py calls it on a
merged folder, whose runs are symlinks into the runs they came from."""
import json, re, struct, sys
from pathlib import Path
out, gen = Path(sys.argv[1]), Path(sys.argv[2])
PHASES = ("search", "index", "insert", "delete", "clustered")
CLASSIC = PHASES[:4]   # "sum" stays the sum of these, as before the clustered phase
# every row says which data set it ran on
with open(gen, "rb") as f:
    seed = struct.unpack_from("<Q", f.read(16), 8)[0]
DATASET = {"dataset": gen.name, "seed": f"{seed:08X}"}

def params(notes):
    """key=value,key=value -> dict, numbers as numbers"""
    out = {}
    for kv in filter(None, notes.split(",")):
        k, _, v = kv.partition("=")
        try:
            out[k] = int(v)
        except ValueError:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out

# diskarray prints the drives' traffic per phase from /proc/diskstats
DRIVES = re.compile(r"drives during (\w+): read ([0-9.]+) GiB \(([0-9.]+) GiB/s\)\s+write ([0-9.]+) GiB \(([0-9.]+) GiB/s\)\s+total ([0-9.]+) GiB/s")

# results: one row per run and operation; phase = extents (1G calibration) or sweep
results = []
for d in sorted((out / "runs").glob("*/*")):
    m = re.match(r"(.+?)-([0-9.]+[KMG]iB)(?:-e(\d+))?$", d.name)
    text = (d / "output.txt").read_text()
    exit_line = re.search(r"^exit (\d+) elapsed ([0-9.]+) s(?: peak_rss_kib (\d+))?", text, re.M)
    tunables = re.match(r"\$ GLIBC_TUNABLES=(\S+) ", text)
    drives = {g[0]: {"drive_read_gib": float(g[1]), "drive_read_gib_s": float(g[2]), "drive_write_gib": float(g[3]),
                     "drive_write_gib_s": float(g[4]), "drive_total_gib_s": float(g[5])} for g in DRIVES.findall(text)}
    for f in d.glob("*.json"):
        j = json.loads(f.read_text())
        for ev, us, ops, nodes, notes in zip(j["event"], j["us"], j["ops"], j["nodes"], j["notes"]):
            if ev not in PHASES:
                continue
            # ops 0: the phase was skipped (libart -R index), recorded as n/a
            results.append({"phase": d.parent.name, "structure": m[1], "size": m[2],
                            "extent": int(m[3]) if m[3] else None, "nodes": nodes, "event": ev,
                            "ns_op": us * 1000 / ops if ops else None, "skipped": not ops, "ops": ops,
                            "notes": notes, "params": params(notes), **DATASET,
                            "run": d.resolve().parents[2].name,  # differs from out in a merged folder
                            "malloc": tunables[1] if tunables else "plain",
                            "elapsed_s": float(exit_line[2]) if exit_line else None,
                            "peak_rss_gib": int(exit_line[3]) / 2**20 if exit_line and exit_line[3] else None,
                            **drives.get(ev, {})})
(out / "results.json").write_text(json.dumps(results, indent=1) + "\n")

def table(rows, key, label):
    by = {}
    for r in rows:
        by.setdefault(key(r), {})[r["event"]] = r["ns_op"]
    rss = {key(r): r.get("peak_rss_gib") for r in rows}
    print(f"  {label:>12} " + "".join(f"{p:>10}" for p in PHASES) + f"{'sum':>10}{'rss GiB':>10}")
    for k, v in by.items():
        cells = "".join(f"{'n/a':>10}" if p in v and v[p] is None else f"{v[p]:10.0f}" if p in v else f"{'':>10}" for p in PHASES)
        total = f"{sum(v[p] for p in CLASSIC):.0f}" if all(v.get(p) is not None for p in CLASSIC) else ""
        print(f"  {str(k):>12} {cells}{total:>10}{'' if rss.get(k) is None else f'{rss[k]:10.2f}'}")

cal = [r for r in results if r["phase"] == "extents"]
if cal:
    print("extent calibration at 1G nodes (ns/op)")
    for s in dict.fromkeys(r["structure"] for r in cal):
        print(f"\n{s}")
        table(sorted((r for r in cal if r["structure"] == s), key=lambda r: r["extent"]), lambda r: r["extent"], "extent")
    print("\nchosen:", (out / "extents.env").read_text().replace("\n", "  "))

sweep = [r for r in results if r["phase"] == "sweep"]
if sweep:
    print("\nsweep (ns/op)")
    for s in dict.fromkeys(r["structure"] for r in sweep):
        rows = sorted((r for r in sweep if r["structure"] == s), key=lambda r: r["nodes"])
        e = rows[0]["extent"]
        print(f"\n{s}{f' (extent {e})' if e else ''}")
        table(rows, lambda r: r["nodes"], "nodes")
        if any("drive_total_gib_s" in r for r in rows):
            print("  drive traffic, GiB/s read + write")
            by = {}
            for r in rows:
                if "drive_total_gib_s" in r:
                    by.setdefault(r["nodes"], {})[r["event"]] = f'{r["drive_read_gib_s"]:.2f}+{r["drive_write_gib_s"]:.2f}'
            print(f"  {'nodes':>12} " + "".join(f"{p:>12}" for p in PHASES))
            for k, v in by.items():
                print(f"  {k:>12} " + "".join(f"{v.get(p, ''):>12}" for p in PHASES))
runs = sorted({r["run"] for r in results})
if runs != [out.name]:
    print("\nruns: " + "; ".join(f"{s}=" + ",".join(sorted({r['run'] for r in results if r['structure'] == s}))
                                for s in dict.fromkeys(r["structure"] for r in results)))
print(f"\ndata set: {DATASET['dataset']} (seed {DATASET['seed']})")
print("malloc: " + ", ".join(sorted({f"{r['structure']}={r['malloc']}" for r in results})))
print(f"chart data: {out / 'results.json'}")
