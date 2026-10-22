#!/usr/bin/env bash
# The part3 Big-O sweep: every structure over node counts doubling from 2K
# (a 16 KiB list) to 1G (8 GiB), and diskarray on to 64G (512 GiB), the same
# runs as each */sweep.py, with the extent-style knob fixed per structure.
# All sizes come from one generated file (create_nodes/fast_nodes, see
# common/inc/gen_nodes.h): N nodes is its first N values, read O_DIRECT.
#
#   preflight   no stray benchmark processes, system snapshot, just sysinfo,
#               the generated file holds the largest size asked for
#   bring-up    (diskarray only) fabric up + connected if the namespaces are
#               missing, just ddio-enable, drive safety check
#   build       make here
#   extents     each structure with an extent/leaf knob at 1G nodes over
#               EXTENTS_<name>; the one with the lowest
#               search+index+insert+delete ns/op is used for the sweep.
#               Written to extents.env.
#   sweep       each structure over the sizes, ascending; a structure stops
#               after its first run that fails, or
#                 pass  a run still going PASS_TIMEOUT s (default 1800) after
#                       its timed phases start ("--- search benchmarks ---",
#                       after loading, the build and the untimed mean/stddev
#                       walk) is killed (not diskarray)
#                 RAM   twice the last run's peak RSS, the next doubling, would
#                       not fit in RAM_LIMIT_GIB (not diskarray)
#                 size  every structure runs MIN_LOG2..MAX_LOG2 (default 11..36);
#                       diskarray has no time limit
#               HARD_TIMEOUT kills any hung run, load included (0 = never).
#
# Structures and their command lines:
#   avl linkedlist sortedarray art   ./benchmark            (DRAM)
#   libart                           ./benchmark -R         (DRAM; libart has no rank, its
#                                                            median/index are O(n), so index shows n/a)
#   bptree                           ./benchmark -e LEAF -b 16
#   bptree2                          ./batched_bench -g 32 -D 4 -e LEAF -b 16
#                                    (bptree's peer of nextarray2: u64 keys, group resolve)
#   nextarray                        ./benchmark -e E
#   nextarray2                       ./batched_bench -p pipeline -g 32 -D 4 -e E
#                                    (nextarray2/benchmark is nextarray's)
#   diskarray                        sudo ./batched_bench -t targets.yml -w 28 -e E
#                                    WRITES EVERY DRIVE IN targets.yml
# linkedlist and sortedarray stop at 128M nodes like their sweep.py, the other
# The generated file caps every structure at 2^36 (64G nodes).  diskarray
# keeps its pool at 6144 x 4 KiB = 24 MiB (sized for L3) as E changes, and the
# batch at no more than a third of the buffers.
#
#   CONFIRM=1 ./run_part3.sh                         everything
#   PHASES=extents CONFIRM=1 ./run_part3.sh          only test the extent sizes
#   EXTENTS_FROM=results/<ts>/extents.env ./run_part3.sh   sweep with earlier picks
#   STRUCTS="nextarray nextarray2" EXTENT_nextarray=512 ./run_part3.sh
#
# Env: STRUCTS (default all of the above but libart, which matches art)  PHASES (default "extents sweep")
#      PASS_TIMEOUT (default 1800)  HARD_TIMEOUT (default 0)
#      RAM_LIMIT_GIB (default 90% of MemTotal)
#      EXTENTS_<name> (calibration list)  EXTENT_<name> (fix it, skip calibration)
#      EXTENTS_FROM (extents.env from an earlier run)  WARGY (wargy_nvmeof repo)
#      GEN (the generated node file)  MIN_LOG2 / MAX_LOG2 (smallest / largest sweep size, log2 nodes)
#      MALLOC_TUNABLES (default glibc.malloc.hugetlb=1; "" for plain malloc)
# Everything is logged to results/<timestamp>/: runs/<phase>/<name>-<size>-e<E>/
# holds each run's output and JSON; summary.txt is the table, results.json
# its rows (a JSON list) for charts.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$(dirname "$HERE")"
# WARGY: the NVMe-oF lab tooling (fabric bring-up, DDIO, sysinfo). Only diskarray needs it;
# the in-RAM structures run without it.
WARGY="${WARGY:-$HOME/wargy_nvmeof}"
# THP is madvise-only here and nextarray madvises its extents into 2 MiB
# pages; this makes glibc's malloc do the same for everything it hands out,
# so the malloc-per-node structures (avl, art, bptree, ...) aren't paying a
# TLB miss with every cache miss that nextarray doesn't.  sudo drops it, so
# it does not reach diskarray (whose data is on the drives anyway).
MALLOC_TUNABLES="${MALLOC_TUNABLES-glibc.malloc.hugetlb=1}"
STRUCTS="${STRUCTS:-avl linkedlist sortedarray bptree bptree2 art nextarray nextarray2 diskarray}"
PHASES="${PHASES:-extents sweep}"
PASS_TIMEOUT="${PASS_TIMEOUT:-1800}"
HARD_TIMEOUT="${HARD_TIMEOUT:-0}"
RAM_LIMIT_GIB="${RAM_LIMIT_GIB:-$(awk '/^MemTotal:/ {printf "%d", $2 * 0.9 / 1048576}' /proc/meminfo)}"
TARGETS="${TARGETS:-$WARGY/state/targets.yml}"   # the drives diskarray uses; it overwrites every one
GEN="$(realpath -m "${GEN:-$BENCH/data/nodes-gen-5EED0001-512GiB.bin}")"
MIN_LOG2="${MIN_LOG2:-11}"
MAX_LOG2="${MAX_LOG2:-36}"
CAL_LOG2=30   # extent calibration at 1G nodes
OUT="$HERE/results/$(date +%Y%m%d-%H%M%S)"

: "${EXTENTS_bptree:=128 256 512 1024 2048}"
: "${EXTENTS_bptree2:=128 256 512 1024 2048}"
: "${EXTENTS_nextarray:=128 256 512 1024 2048}"
: "${EXTENTS_nextarray2:=128 256 512 1024 2048}"
: "${EXTENTS_diskarray:=256 512 1024 2048}"
# the default each binary uses when nothing else picks one
declare -A DEFAULT_EXTENT=([bptree]=512 [bptree2]=512 [nextarray]=512 [nextarray2]=512 [diskarray]=512)

has() { [[ " $STRUCTS " == *" $1 "* ]]; }
phase() { [[ " $PHASES " == *" $1 "* ]]; }
if { has diskarray; } && [ "${CONFIRM:-}" != "1" ]; then
    echo "ERROR: diskarray overwrites every drive in targets.yml; re-run with CONFIRM=1 (or leave it out of STRUCTS)" >&2
    exit 1
fi

mkdir -p "$OUT"
exec > >(tee -a "$OUT/run.log") 2>&1
# stopping the script stops the run in progress too (bench passes it on)
trap 'echo "interrupted"; pkill -TERM -P $$ -f "run_part3|bash" 2>/dev/null; exit 130' INT TERM

wj() { just --justfile "$WARGY/Justfile" --working-directory "$WARGY" "$@"; }
step() { echo; echo "════ $(date +%T) $*"; }
log_msr() { sudo modprobe msr; echo "MSR 0xC8B (DDIO LLC ways), count per value:"; sudo rdmsr -a 0xC8B | sort | uniq -c; }

# sizes are log2 of the node count: 11 (2K nodes, a 16 KiB list) up
max_log2() { echo "$MAX_LOG2"; }
# diskarray runs to MAX_LOG2 with no time limit; everything else has the pass timeout and RAM rule
limited() { [ "$1" != diskarray ]; }
# the old list-file name for 2^k nodes, e.g. 16.0KiB, 128MiB, 8.00GiB, 512GiB
label() {
    awk -v b="$(( (1 << $1) * 8 ))" 'BEGIN {
        split("KiB MiB GiB TiB", u, " "); i = 0; v = b
        while (v >= 1024 && i < 4) { v /= 1024; i++ }
        printf(v >= 100 ? "%d%s\n" : v >= 10 ? "%.1f%s\n" : "%.2f%s\n", v, u[i]) }'
}
uses_extent() { [ -n "${DEFAULT_EXTENT[$1]:-}" ]; }

# cmd NAME NODES EXTENT: the command line for one run, one word per line
cmd() {
    local name=$1 nodes=$2 e=$3 bufs batch
    case $name in
        avl|linkedlist|sortedarray|art)
            printf '%s\n' "$BENCH/$name/benchmark" -i "$GEN" -c "$nodes" ;;
        libart)
            printf '%s\n' "$BENCH/libart/benchmark" -i "$GEN" -c "$nodes" -R ;;
        bptree)
            printf '%s\n' "$BENCH/bptree/benchmark" -i "$GEN" -c "$nodes" -e "$e" -b 16 ;;
        bptree2)
            printf '%s\n' "$BENCH/bptree2/batched_bench" -i "$GEN" -c "$nodes" -e "$e" -b 16 -g 32 -D 4 ;;
        nextarray)
            printf '%s\n' "$BENCH/nextarray/benchmark" -i "$GEN" -c "$nodes" -e "$e" ;;
        nextarray2)
            printf '%s\n' "$BENCH/nextarray2/batched_bench" -i "$GEN" -c "$nodes" -e "$e" -p pipeline -g 32 -D 4 ;;
        diskarray)
            bufs=$((6144 * 512 / e)); batch=$((bufs / 3 < 2048 ? bufs / 3 : 2048))
            printf '%s\n' sudo "$BENCH/diskarray/batched_bench" -i "$GEN" -c "$nodes" -t "$TARGETS" \
                -e "$e" -w 28 -b "$bufs" --batch-size "$batch" ;;
    esac
}

# bench PHASE NAME LOG2 EXTENT: one run of 2^LOG2 nodes in its own directory
# (the binaries drop their JSON in the cwd).  Prints the elapsed seconds;
# returns the run's exit status, or 99 if the pass timeout killed it.
bench() {
    local ph=$1 name=$2 size e=$4 dir args rc t0 t1
    size=$(label "$3")
    dir="$OUT/runs/$ph/$name-$size${e:+-e$e}"
    mkdir -p "$dir"
    mapfile -t args < <(cmd "$name" "$((1 << $3))" "$e")
    local tun=$MALLOC_TUNABLES
    [ "${args[0]}" = sudo ] && tun=""      # sudo resets the environment
    echo "\$ ${tun:+GLIBC_TUNABLES=$tun }${args[*]}" > "$dir/output.txt"
    t0=$(date +%s.%N)
    rc=0
    # peak RSS (KiB) lands in maxrss for the RAM stop rule; stdout line-buffered
    # so the "--- search benchmarks ---" line shows up as soon as it is printed
    (cd "$dir" && { [ -z "$tun" ] || export GLIBC_TUNABLES="$tun"; } && exec /usr/bin/time -o "$dir/maxrss" -f %M timeout -k 30 "$HARD_TIMEOUT" stdbuf -oL "${args[@]}") >> "$dir/output.txt" 2>&1 &
    local run=$! loaded="" killed=""
    # the run is a background job, which ignores Ctrl-C: take it down with us
    trap 'pkill -TERM -P "$run" -x timeout 2>/dev/null; wait "$run" 2>/dev/null; exit 130' INT TERM
    if limited "$name" && [ "$PASS_TIMEOUT" -gt 0 ]; then
        while kill -0 "$run" 2>/dev/null; do
            sleep 1
            # the clock starts with the timed phases, after the build
            if [ -z "$loaded" ] && grep -qi "^--- search benchmarks ---" "$dir/output.txt"; then
                loaded=$(date +%s)
                echo "timed phases started after $(( loaded - ${t0%.*} )) s" >> "$dir/output.txt"
            fi
            if [ -n "$loaded" ] && (( $(date +%s) - loaded > PASS_TIMEOUT )); then
                # signal timeout (time's child): it passes it to the benchmark's
                # process group and time still records the peak RSS
                pkill -TERM -P "$run" -x timeout 2>/dev/null || true
                killed=1
                break
            fi
        done
    fi
    wait "$run" || rc=$?
    trap - INT TERM
    if [ -n "$killed" ]; then
        echo "pass timeout: still running $PASS_TIMEOUT s after the timed phases started; killed" >> "$dir/output.txt"
        rc=99
    fi
    t1=$(date +%s.%N)
    [ "${args[0]}" = sudo ] && sudo chown -R "$(id -u):$(id -g)" "$dir"
    local el; el=$(echo "$t1 - $t0" | bc)
    echo "exit $rc elapsed $el s peak_rss_kib $(tail -1 "$dir/maxrss" 2>/dev/null)" >> "$dir/output.txt"
    grep -E "^\s+(search|index|insert|delete|clustered):\s" "$dir/output.txt" | sed "s/^/  /" >&2 || true
    printf "  %s %s%s: exit %d, %.1f s\n" "$name" "$size" "${e:+ e=$e}" "$rc" "$el" >&2
    echo "$el"
    return "$rc"
}

# ns/op sum over search+index+insert+delete from a run's JSON, empty if missing
score() {
    python3 - "$1" <<'EOF'
import json, sys
from pathlib import Path
for f in Path(sys.argv[1]).glob("*.json"):
    d = json.loads(f.read_text())
    ns = {e: u * 1000 / o for e, u, o in zip(d["event"], d["us"], d["ops"]) if o}
    if all(p in ns for p in ("search", "index", "insert", "delete")):
        print(f'{sum(ns[p] for p in ("search", "index", "insert", "delete")):.1f}')
    break
EOF
}

echo "malloc: ${MALLOC_TUNABLES:-plain}  THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "structures: $STRUCTS  phases: $PHASES  RAM limit: $RAM_LIMIT_GIB GiB  pass timeout: $PASS_TIMEOUT s (not diskarray)  results: $OUT"
if has diskarray; then
    sudo -v
    # keep sudo alive through the long runs; diskarray needs it
    while sleep 60; do sudo -n true; done 2>/dev/null &
    SUDO_KEEPALIVE=$!
    trap 'kill $SUDO_KEEPALIVE 2>/dev/null' EXIT
fi

# ── preflight ─────────────────────────────────────────────────────────

step "preflight: stray benchmark processes"
# a hung run left on cpu 0 skews everything after it
if pgrep -a -f '/(benchmark|batched_bench)( |$)'; then
    echo "ERROR: benchmark processes already running (above); kill them first" >&2
    exit 1
fi

step "preflight: system snapshot"
{
    date -Is; uname -a; lscpu | grep -E "Model name|Socket|Core|Thread|NUMA node\(s\)|L3"
    free -g
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    echo "thp: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
    echo "content: $(git -C "$BENCH" rev-parse --short HEAD) $(git -C "$BENCH" status --short . | wc -l) dirty"
    echo "wargy:   $(git -C "$WARGY" rev-parse --short HEAD) $(git -C "$WARGY" status --short | wc -l) dirty"
} | tee "$OUT/system.txt"
INITIATOR=$(awk '/- name:/ {n=$3} /role: initiator/ {print n; exit}' "$WARGY/state/cluster.yml" 2>/dev/null || true)
run_sysinfo() { env OUT="$OUT/sysinfo/$1" HOST="$INITIATOR" just --justfile "$WARGY/Justfile" --working-directory "$WARGY" sysinfo > "$OUT/sysinfo_$1.txt" 2>&1 || echo "WARNING: just sysinfo failed (sysinfo_$1.txt)"; }
step "preflight: just sysinfo on $INITIATOR"
run_sysinfo preflight

step "preflight: generated node file"
# the header's count (bytes 16..23) must cover the largest run
top=0
for name in $STRUCTS; do m=$(max_log2 "$name"); [ "$m" -gt "$top" ] && top=$m; done
have=$(od -An -tu8 -j16 -N8 "$GEN" 2>/dev/null | tr -d ' ' || true)
if [ "$(head -c 8 "$GEN" 2>/dev/null)" != BFNODES1 ] || [ "${have:-0}" -lt $(( (1 << top) | 1 )) ]; then
    echo "ERROR: $GEN must be a fast_nodes file of at least 2^$top nodes; make it with" >&2
    echo "       (cd $BENCH/create_nodes && ./fast_nodes -c $(( 1 << top )) -o ../data)" >&2
    exit 1
fi
echo "$GEN: $have nodes, runs up to 2^$top"

# ── bring-up ──────────────────────────────────────────────────────────

# the NVMe-oF namespaces on this host: nvmeXcYnZ with an rdma controller -> nvmeXnZ
fabric_namespaces() {
    for c in /sys/block/nvme*c*n*; do
        [ -e "$c" ] && [ "$(cat "$c/device/transport" 2>/dev/null)" = rdma ] && basename "$c" | sed 's/c[0-9]*n/n/'
    done | sort -u
}

if has diskarray; then
    step "bring-up: NVMe-oF drives for diskarray"
    if [ -z "$(fabric_namespaces)" ]; then
        wj fabric-up | tee "$OUT/fabric_up.txt"
        wj export | tee "$OUT/export.txt"
        wj connect | tee "$OUT/connect.txt"
    fi
    cp "$TARGETS" "$OUT/targets.yml"
    want=$(grep -c "serial:" "$TARGETS")
    mapfile -t NS < <(fabric_namespaces)
    echo "targets.yml drives: $want  fabric namespaces: ${NS[*]}"
    [ "${#NS[@]}" -eq "$want" ] || { echo "ERROR: ${#NS[@]} fabric namespaces, targets.yml lists $want" >&2; exit 1; }
    # diskarray overwrites them: refuse if anything has one mounted or in an md array
    for n in "${NS[@]}"; do lsblk -dno NAME,SIZE,MODEL,SERIAL "/dev/$n"; done | tee "$OUT/drives.txt"
    for n in "${NS[@]}"; do
        if lsblk -nro MOUNTPOINTS "/dev/$n" | grep -q . || grep -qw "$n" /proc/mdstat; then
            echo "ERROR: /dev/$n is mounted or in an md array; not writing to it" >&2
            exit 1
        fi
    done
fi

if has diskarray; then
    step "bring-up: DDIO"
    wj ddio-enable | tee "$OUT/ddio_enable.txt"
    log_msr | tee "$OUT/msr_before.txt"
fi

# ── build ─────────────────────────────────────────────────────────────

step "build"
make -C "$BENCH"

# ── extents: pick each structure's extent size at 1G nodes ────────────

declare -A EXTENT
for name in $STRUCTS; do uses_extent "$name" && EXTENT[$name]=${DEFAULT_EXTENT[$name]}; done
if [ -n "${EXTENTS_FROM:-}" ]; then
    # shellcheck disable=SC1090
    source "$EXTENTS_FROM"
fi
for name in "${!EXTENT[@]}"; do
    v="EXTENT_$name"; [ -n "${!v:-}" ] && EXTENT[$name]=${!v}
done

if phase extents; then
    for name in $STRUCTS; do
        uses_extent "$name" || continue
        v="EXTENT_$name"
        if [ -n "${!v:-}" ]; then echo "extents: $name fixed at ${!v}"; continue; fi
        list="EXTENTS_$name"
        best="" best_score=""
        for e in ${!list}; do
            step "extents: $name e=$e at $(label $CAL_LOG2)"
            bench extents "$name" "$CAL_LOG2" "$e" >/dev/null || { echo "  (failed, see output.txt)"; continue; }
            s=$(score "$OUT/runs/extents/$name-$(label $CAL_LOG2)-e$e")
            echo "  sum of search+index+insert+delete: ${s:-?} ns/op"
            if [ -n "$s" ] && { [ -z "$best_score" ] || (( $(echo "$s < $best_score" | bc) )); }; then
                best=$e best_score=$s
            fi
        done
        if [ -n "$best" ]; then
            EXTENT[$name]=$best
            echo "extents: $name -> $best ($best_score ns/op summed)"
        else
            echo "WARNING: no extent run of $name finished; sweeping at ${EXTENT[$name]}"
        fi
    done
fi
for name in "${!EXTENT[@]}"; do echo "EXTENT_$name=${EXTENT[$name]}"; done | sort > "$OUT/extents.env"
echo "extent sizes for the sweep:"; sed 's/^/  /' "$OUT/extents.env"

# ── sweep: each structure over the sizes ──────────────────────────────

if phase sweep; then
    for name in $STRUCTS; do
        e=""
        uses_extent "$name" && e=${EXTENT[$name]}
        for ((k = MIN_LOG2; k <= $(max_log2 "$name"); k++)); do
            size=$(label $k)
            step "sweep: $name $size${e:+ e=$e}"
            rc=0
            el=$(bench sweep "$name" "$k" "$e") || rc=$?
            if [ "$rc" = 99 ]; then
                echo "sweep: $name ran $PASS_TIMEOUT s into its timed phases at $size; stopping $name" | tee -a "$OUT/stopped.txt"
                break
            elif [ "$rc" != 0 ]; then
                echo "sweep: $name failed at $size (exit $rc); stopping $name" | tee -a "$OUT/stopped.txt"
                break
            fi
            if limited "$name"; then
                rss=$(tail -1 "$OUT/runs/sweep/$name-$size${e:+-e$e}/maxrss")
                gib=$(awk -v k="$rss" 'BEGIN {printf "%.2f", k / 1048576}')
                echo "  peak RSS $gib GiB"
                if (( 2 * rss > RAM_LIMIT_GIB * 1048576 )); then
                    echo "sweep: $name peaked at $gib GiB at $size; the next doubling would not fit in $RAM_LIMIT_GIB GiB; stopping $name" | tee -a "$OUT/stopped.txt"
                    break
                fi
            fi
        done
    done
fi

if has diskarray; then log_msr | tee "$OUT/msr_after.txt"; fi
step "just sysinfo after"
run_sysinfo after

# ── summary ───────────────────────────────────────────────────────────

step "summary"
python3 "$HERE/summarize.py" "$OUT" "$GEN" | tee "$OUT/summary.txt"
echo "results: $OUT"
