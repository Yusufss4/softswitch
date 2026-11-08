# shellcheck shell=bash
# Helpers for netns integration tests. Each test sources this file, calls `setup` and then
# uses the assertions; `teardown` runs on exit no matter what.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO_ROOT"
source scripts/topology.sh

FAILURES=0
TEST_NAME=$(basename "$0" .sh)

log() { printf '[%s] %s\n' "$TEST_NAME" "$*"; }

teardown() {
  local rc=$?
  if [[ $rc -ne 0 || $FAILURES -ne 0 ]]; then
    log "---- logs (failure) ----"
    for f in "$RUN_DIR"/*.log; do
      [[ -f "$f" ]] && { echo "== $f"; tail -n 30 "$f"; }
    done
  fi
  scripts/switches.sh stop >/dev/null 2>&1 || true
  scripts/topo.sh down >/dev/null 2>&1 || true
}

# setup: build the topology and start every switch. TOPO_LOOP=1 also brings up the sw2-sw3 trunk.
setup() {
  trap teardown EXIT
  scripts/switches.sh stop >/dev/null 2>&1 || true
  scripts/topo.sh down >/dev/null 2>&1 || true
  rm -f "$RUN_DIR"/*.log
  if [[ "${TOPO_LOOP:-0}" == "1" ]]; then
    scripts/topo.sh up --loop
  else
    scripts/topo.sh up
  fi
  scripts/switches.sh start
}

in_host() {
  local h=$1
  shift
  sudo ip netns exec "$h" "$@"
}

pass() { log "PASS: $*"; }
fail() {
  log "FAIL: $*"
  FAILURES=$((FAILURES + 1))
}

# expect_ping SRC DST_HOST_OR_IP [count]
expect_ping() {
  local src=$1 dst=$2 count=${3:-3}
  local ip=$dst
  [[ -n "${HOSTS[$dst]:-}" ]] && ip=$(host_ip "$dst")
  if in_host "$src" ping -c "$count" -i 0.2 -W 1 -q "$ip" >/dev/null 2>&1; then
    pass "$src can reach $dst ($ip)"
  else
    fail "$src cannot reach $dst ($ip)"
  fi
}

# expect_no_ping SRC DST_HOST_OR_IP
expect_no_ping() {
  local src=$1 dst=$2
  local ip=$dst
  [[ -n "${HOSTS[$dst]:-}" ]] && ip=$(host_ip "$dst")
  if in_host "$src" ping -c 2 -i 0.2 -W 1 -q "$ip" >/dev/null 2>&1; then
    fail "$src unexpectedly reached $dst ($ip)"
  else
    pass "$src is isolated from $dst ($ip)"
  fi
}

# Like ping, but waits up to $3 seconds for the path to come up (e.g. STP convergence).
wait_for_ping() {
  local src=$1 dst=$2 timeout=${3:-10}
  local ip=$dst
  [[ -n "${HOSTS[$dst]:-}" ]] && ip=$(host_ip "$dst")
  local deadline=$((SECONDS + timeout))
  while ((SECONDS < deadline)); do
    if in_host "$src" ping -c 1 -W 1 -q "$ip" >/dev/null 2>&1; then
      pass "$src reached $dst after $((timeout - (deadline - SECONDS)))s"
      return 0
    fi
    sleep 0.2
  done
  fail "$src never reached $dst within ${timeout}s"
}

expect_eq() {
  if [[ "$1" == "$2" ]]; then pass "$3"; else fail "$3 (got '$1', want '$2')"; fi
}

# forward_busy SW: percent of one CPU that SW's ss-forward thread used over the next second.
# An idle switch sleeps in poll() and stays near 0; a spinning one shows ~100.
forward_busy() {
  local pid tid t1 t2
  for pid in $(pgrep -x ss-dataplane); do
    tr '\0' ' ' <"/proc/$pid/cmdline" | grep -qE -- "--name $1( |$)" && break
    pid=""
  done
  [[ -n "$pid" ]] || { echo "-1"; return; }
  tid=$(ps -L -o tid=,comm= -p "$pid" | awk '$2=="ss-forward"{print $1}')
  t1=$(awk '{print $14 + $15}' "/proc/$pid/task/$tid/stat")
  sleep 1
  t2=$(awk '{print $14 + $15}' "/proc/$pid/task/$tid/stat")
  echo $(((t2 - t1) * 100 / $(getconf CLK_TCK)))
}

finish() {
  if [[ $FAILURES -ne 0 ]]; then
    log "$FAILURES assertion(s) failed"
    exit 1
  fi
  log "all assertions passed"
}
