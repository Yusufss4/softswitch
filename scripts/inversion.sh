#!/usr/bin/env bash
# shellcheck disable=SC2024  # output files are redirected by the user's shell on purpose, so they stay user-owned
# Priority-inversion demo: why the data plane never shares a lock with the control plane.
#
#   scripts/inversion.sh            results in reports/inversion/
#
# For each publish mode (qsbr, mutex, mutex-pi) the switch forwards 20 kpps of 64-byte frames
# while the control plane republishes its tables 1000 times a second (--publish-hz), and a
# SCHED_FIFO 30 hog on the control plane's CPU spins 20 ms out of every 50 ms. The data plane
# runs at SCHED_FIFO 50 on its own CPU.
#
#   qsbr      the data plane never waits for the control plane; the hog only delays publishes.
#   mutex     the hog preempts the SCHED_OTHER control plane while it holds the table lock, so
#             the FIFO 50 data plane waits behind a FIFO 30 task: inversion, up to 20 ms.
#   mutex-pi  priority inheritance lifts the holder to FIFO 50 while the data plane waits, so
#             it runs ahead of the hog and the wait is bounded by the critical section.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/bench_lib.sh
source scripts/bench_lib.sh
OUT=${INVERSION_OUT:-reports/inversion}
RESULTS="$OUT/results.jsonl"
DURATION=${DURATION:-10}
RATE=${RATE:-20000}
MODES=${MODES:-qsbr mutex mutex-pi}
PUBLISH_HZ=${PUBLISH_HZ:-1000}
HOG=${HOG:-1}  # 0 runs without the hog (control experiment)
mkdir -p "$OUT"
: >"$RESULTS"
trap cleanup EXIT

topology
for mode in $MODES; do
  echo "== publish mode $mode"
  CP_EXTRA_ARGS="--publish-mode $mode --publish-hz $PUBLISH_HZ" start_softswitch sendmmsg
  hog=""
  if [[ "$HOG" == 1 ]]; then
    sudo "$BIN/ss-hog" --cpu 0 --priority 30 --busy-ms 20 --period-ms 50 \
      --duration "$((DURATION + 3))" >"$RUN/hog.json" &
    hog=$!
  fi
  measure "$mode" 64 "$RATE" "latency-$mode.csv"
  [[ -n "$hog" ]] && wait "$hog"
  "$BIN/sscli" -s bench --run-dir "$RUN" show dataplane | tee "$OUT/dataplane-$mode.txt" |
    grep -E "publish mode|table lock|table publishes|max batch"
  stop_softswitch
done
echo "results: $RESULTS"
