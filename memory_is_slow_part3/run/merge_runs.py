#!/usr/bin/env python3
"""Merge several run_part3.sh results folders into one, for charts.

    merge_runs.py [-o results/unified] [-x <ts> ...] [results/<ts> ...]

With no folders, every results/<ts>/ is a candidate.  A run (one
runs/<phase>/<name>-<size>[-e<E>]/ directory) counts only if it exited 0 and
wrote its JSON; of those, the newest folder wins each phase, structure, size
and extent.  A structure's sweep keeps one extent, the one in its newest run,
so a curve never mixes extents.  -x leaves a folder out.

The output folder holds symlinks, never copies: runs/<phase>/<name> points
into the run it came from, sources/<ts> at each folder used (system.txt,
sysinfo, run.log), plus sources.tsv (which run each came from), extents.env
(the newest pick per structure) and summarize.py's summary.txt and
results.json, whose rows carry "run".  Every run must be on the same data set.
"""
import argparse, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = re.compile(r"(.+?)-([0-9.]+[KMG]iB)(?:-e(\d+))?$")

ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
ap.add_argument("folders", nargs="*", type=Path)
ap.add_argument("-o", "--out", type=Path, default=HERE / "results" / "unified")
ap.add_argument("-x", "--exclude", action="append", default=[], metavar="TS")
args = ap.parse_args()

out = args.out.resolve()
folders = args.folders or [p for p in (HERE / "results").iterdir() if re.fullmatch(r"\d{8}-\d{6}", p.name)]
folders = sorted((f.resolve() for f in folders if f.name not in args.exclude), key=lambda f: f.name)

# (phase, structure, size, extent) -> newest good run directory
picked, skipped, datasets = {}, [], set()
for f in folders:
    for d in sorted((f / "runs").glob("*/*")):
        m = NAME.match(d.name)
        text = (d / "output.txt").read_text() if (d / "output.txt").exists() else ""
        ok = m and re.search(r"^exit 0 ", text, re.M) and any(d.glob("*.json"))
        if not ok:
            skipped.append(d)
            continue
        datasets.update(re.findall(r"-i (\S+)", text.split("\n", 1)[0]))
        picked[(d.parent.name, m[1], m[2], m[3])] = d   # folders ascend, so newest wins

if len({Path(p).name for p in datasets}) > 1:
    sys.exit(f"runs are on different data sets: {sorted(datasets)}")

# one extent per structure's sweep: the newest run's
newest = {}
for (phase, s, size, e), d in picked.items():
    if phase == "sweep" and d.parents[2].name >= newest.get(s, ("", None))[0]:
        newest[s] = (d.parents[2].name, e)
dropped = [k for k in picked if k[0] == "sweep" and k[3] != newest[k[1]][1]]
for k in dropped:
    del picked[k]

if out.exists():
    if not (out / "sources.tsv").exists():
        sys.exit(f"{out} exists and is not a merged folder; not touching it")
    shutil.rmtree(out)   # only symlinks and generated files
(out / "sources").mkdir(parents=True)

used = sorted({d.parents[2] for d in picked.values()}, key=lambda f: f.name)
for f in used:
    (out / "sources" / f.name).symlink_to(os.path.relpath(f, out / "sources"))
rows = []
for (phase, s, size, e), d in sorted(picked.items()):
    link = out / "runs" / phase / d.name
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(os.path.relpath(d, link.parent))
    rows.append(f"{phase}\t{d.name}\t{d.parents[2].name}")
(out / "sources.tsv").write_text("phase\trun\tfrom\n" + "\n".join(rows) + "\n")

# extents.env: later folders override earlier picks
env = {}
for f in used:
    if (f / "extents.env").exists():
        for line in (f / "extents.env").read_text().split():
            k, _, v = line.partition("=")
            env[k] = v
(out / "extents.env").write_text("".join(f"{k}={v}\n" for k, v in sorted(env.items())))

print(f"{len(picked)} runs from {len(used)} folders; {len(skipped)} failed or unfinished runs left out", file=sys.stderr)
for k in dropped:
    print(f"  dropped {k[1]}-{k[2]}-e{k[3]}: its newest sweep is e{newest[k[1]][1]}", file=sys.stderr)

gen = HERE.parent / "data" / Path(datasets.pop()).name if datasets else None
summary = subprocess.run([sys.executable, str(HERE / "summarize.py"), str(out), str(gen)],
                         check=True, capture_output=True, text=True).stdout
(out / "summary.txt").write_text(summary)
print(summary, end="")
