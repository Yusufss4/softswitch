#!/usr/bin/env bash
# Starts or stops softswitchd + ss-dataplane for every switch in the test topology.
# Usage: scripts/switches.sh start [sw1 sw2 ...] | stop | status
# Env: SOFTSWITCH_BIN_DIR (default build/release), RUN_DIR (default /tmp/softswitch),
#      DP_ARGS (extra ss-dataplane arguments), CP_ARGS (extra softswitchd arguments).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/topology.sh

ncpu=$(nproc)

start_one() {
  local sw=$1 index=$2
  local cpu=$(((index + 1) % ncpu))
  mkdir -p "$RUN_DIR"
  local config_args=()
  [[ -f "configs/$sw.conf" ]] && config_args=(--config "configs/$sw.conf")
  # shellcheck disable=SC2086
  "$BIN_DIR/softswitchd" --name "$sw" --ports "${SWITCH_PORTS[$sw]}" --run-dir "$RUN_DIR" \
    "${config_args[@]}" ${CP_ARGS:-} >"$RUN_DIR/$sw.cp.log" 2>&1 &
  echo $! >"$RUN_DIR/$sw.cp.pid"
  for _ in $(seq 50); do
    [[ -S "$RUN_DIR/$sw.dp.sock" ]] && break
    sleep 0.1
  done
  # Logs stay owned by the invoking user, hence the redirect outside sudo.
  # shellcheck disable=SC2086,SC2024
  sudo "$BIN_DIR/ss-dataplane" --socket "$RUN_DIR/$sw.dp.sock" --name "$sw" --cpu "$cpu" \
    ${DP_ARGS:-} >"$RUN_DIR/$sw.dp.log" 2>&1 &
  for _ in $(seq 50); do
    grep -q "data plane attached" "$RUN_DIR/$sw.cp.log" 2>/dev/null && break
    sleep 0.1
  done
  if ! grep -q "data plane attached" "$RUN_DIR/$sw.cp.log"; then
    echo "$sw failed to start:" >&2
    cat "$RUN_DIR/$sw.cp.log" "$RUN_DIR/$sw.dp.log" >&2
    return 1
  fi
  echo "$sw up (data plane on cpu $cpu)"
}

stop_all() {
  sudo pkill -TERM -x ss-dataplane 2>/dev/null || true
  for pidfile in "$RUN_DIR"/*.cp.pid; do
    [[ -f "$pidfile" ]] || continue
    kill "$(cat "$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
  done
  sleep 0.3
}

case "${1:-}" in
  start)
    shift
    targets=("$@")
    [[ ${#targets[@]} -eq 0 ]] && targets=("${SWITCHES[@]}")
    i=0
    for sw in "${targets[@]}"; do
      start_one "$sw" "$i"
      i=$((i + 1))
    done
    ;;
  stop) stop_all ;;
  status) pgrep -a softswitchd || true; pgrep -a ss-dataplane || true ;;
  *) echo "usage: $0 start [switch...] | stop | status" >&2; exit 2 ;;
esac
