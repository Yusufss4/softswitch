#!/usr/bin/env bash
# shellcheck disable=SC2024  # output files are redirected by the user's shell on purpose, so they stay user-owned
# Profiles the forwarding thread under full load: perf stat (IPC, cache and branch misses) and
# perf record with call graphs, folded into a flame graph.
#
#   scripts/perf.sh [sendmmsg|ring|sendto]     results in reports/perf/<mode>/
#
# The thread is found by name (ss-forward), so only forwarding work is measured, not the
# control plane or the generator. scripts/flamegraph.py folds the stacks and draws the SVG.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/bench_lib.sh
source scripts/bench_lib.sh
MODE=${1:-sendmmsg}
PERF_OUT="${PERF_OUT:-reports/perf}/$MODE"
SECONDS_PROFILED=${SECONDS_PROFILED:-5}
mkdir -p "$PERF_OUT"
trap cleanup EXIT

topology
start_softswitch "$MODE"
load_seconds=$((SECONDS_PROFILED * 2 + 6))
sudo ip netns exec bsink "$BIN/ss-sink" --iface eth0 --cpu 2 --duration "$((load_seconds + 1))" >"$PERF_OUT/sink.json" &
sink=$!
sleep 0.5
sudo ip netns exec bgen "$BIN/ss-pktgen" --iface eth0 --cpu 1 --dst-mac "$SINK_MAC" --size 64 --rate 0 \
  --duration "$load_seconds" >"$PERF_OUT/gen.json" &
gen=$!
sleep 2

dp_pid=$(pgrep -x ss-dataplane)
tid=$(ps -L -o tid=,comm= -p "$dp_pid" | awk '$2=="ss-forward"{print $1}')
echo "profiling ss-forward (tid $tid) in $MODE mode for ${SECONDS_PROFILED}s per pass"

sudo perf stat -t "$tid" -e task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses \
  -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses -- sleep "$SECONDS_PROFILED" \
  2>"$PERF_OUT/stat.txt"
sudo perf record -q -t "$tid" -F 4999 --call-graph dwarf,16384 -o "$RUN/perf.data" -- sleep "$SECONDS_PROFILED" 2>/dev/null
wait "$gen" "$sink"

sudo perf report -i "$RUN/perf.data" --no-children --stdio --percent-limit 0.5 --sort symbol \
  -g none 2>/dev/null | grep -v "^$" >"$PERF_OUT/report.txt"
sudo perf script -i "$RUN/perf.data" 2>/dev/null >"$RUN/perf-script.txt"
scripts/flamegraph.py <"$RUN/perf-script.txt" --title "ss-forward, $MODE TX, 64-byte frames at full load" \
  --folded "$PERF_OUT/folded.txt" >"$PERF_OUT/flamegraph.svg"
sudo rm -f "$RUN/perf.data"

cat "$PERF_OUT/stat.txt"
head -40 "$PERF_OUT/report.txt"
echo "flame graph: $PERF_OUT/flamegraph.svg"
