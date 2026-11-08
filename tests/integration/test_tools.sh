#!/usr/bin/env bash
# The benchmark tools and data-plane modes end to end, on the one-switch benchmark topology:
# ss-pktgen -> softswitch -> ss-sink must deliver every frame in each TX mode and in the
# lock-based publish mode used by the priority-inversion demo.
set -euo pipefail
source "$(dirname "$0")/lib.sh"
# shellcheck source=scripts/bench_lib.sh
source scripts/bench_lib.sh
OUT=$(mktemp -d)
RESULTS="$OUT/results.jsonl"
DURATION=1
trap 'cleanup; rm -rf "$OUT"' EXIT

# check_run LABEL: the last measurement delivered every frame with a sane latency.
check_run() {
  local verdict
  verdict=$(python3 -c '
import json, sys
r = json.loads(open(sys.argv[1]).read().splitlines()[-1])
sent, got, p50 = r["gen"]["sent"], r["sink"]["received"], r["sink"]["lat_us"]["p50"]
print("ok" if sent > 0 and got == sent and 0 < p50 < 5000 else f"sent={sent} received={got} p50={p50}us")
' "$RESULTS")
  expect_eq "$verdict" ok "$1: every frame forwarded"
}

topology
for mode in sendmmsg ring sendto; do
  start_softswitch "$mode"
  measure "tx-$mode" 64 5000 >/dev/null
  check_run "TX mode $mode"
  stop_softswitch
done

CP_EXTRA_ARGS="--publish-mode mutex-pi --publish-hz 200" start_softswitch sendmmsg
measure mutex-pi 64 5000 >/dev/null
check_run "publish mode mutex-pi"
dataplane=$("$BIN/sscli" -s bench --run-dir "$RUN" show dataplane)
expect_eq "$(grep -c 'publish mode: *mutex-pi' <<<"$dataplane")" 1 "show dataplane reports the publish mode"
lock_batches=$(awk '/^table lock:/{print $3}' <<<"$dataplane")
expect_eq "$((lock_batches > 0))" 1 "the data plane took the table lock ($lock_batches batches)"
expect_eq "$("$BIN/sscli" -s bench --run-dir "$RUN" show counters | grep -c 'RX ring full')" 1 \
  "show counters reports kernel RX ring drops"
stop_softswitch
finish
