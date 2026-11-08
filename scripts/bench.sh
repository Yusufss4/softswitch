#!/usr/bin/env bash
# Forwarding benchmark on the Pi: one switch between a generator and a sink, each in its own
# netns. Compares softswitch's three TX modes (sendmmsg, TX ring, per-packet sendto) with the
# Linux kernel bridge.
#
#   scripts/bench.sh [--quick]
#
# Writes reports/bench/results.jsonl (fixed-rate and saturation runs), ndr.jsonl (no-drop
# rates) and latency-*.csv histograms; scripts/bench_report.py turns them into tables.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/bench_lib.sh
source scripts/bench_lib.sh
[[ "${1:-}" == "--quick" ]] && DURATION=2 && NDR_STEPS=5
: >"$OUT/results.jsonl"
: >"$OUT/ndr.jsonl"
trap cleanup EXIT
topology

echo "== softswitch, sendmmsg (default)"
start_softswitch sendmmsg
for size in 64 512 1500; do measure softswitch "$size" 0; done
for size in 64 512 1500; do ndr softswitch "$size"; done
measure softswitch 64 10000 latency-softswitch-10k.csv
measure softswitch 64 100000 latency-softswitch-100k.csv
stop_softswitch

for mode in ring sendto; do
  echo "== softswitch, $mode"
  start_softswitch "$mode"
  measure "softswitch-$mode" 64 0
  ndr "softswitch-$mode" 64
  stop_softswitch
done

echo "== Linux bridge (baseline)"
start_bridge
for size in 64 512 1500; do measure linux-bridge "$size" 0; done
for size in 64 512 1500; do ndr linux-bridge "$size"; done
measure linux-bridge 64 10000 latency-bridge-10k.csv
measure linux-bridge 64 100000 latency-bridge-100k.csv
stop_bridge

echo "results: $OUT/results.jsonl"
