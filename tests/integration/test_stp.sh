#!/usr/bin/env bash
# M3: RSTP on the looped 3-switch triangle: convergence, no storm, sub-second failover.
TOPO_LOOP=1
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
alternates() {
  local n=0
  for sw in sw1 sw2 sw3; do
    n=$((n + $(cli "$sw" show spanning-tree | awk '$2=="alternate"' | wc -l)))
  done
  echo "$n"
}
root_of() { cli "$1" show spanning-tree | awk '/^Root ID:/ {print $3}'; }
role_of() { cli "$1" show spanning-tree | awk -v p="$2" '$1==p {print $2}'; }
rx_total() { cli "$1" show dataplane | awk '/^rx\/tx packets:/ {print $3}'; }
wait_alternates() {
  local want=$1 deadline=$((SECONDS + 5))
  while ((SECONDS < deadline)); do
    [[ "$(alternates)" == "$want" ]] && return 0
    sleep 0.2
  done
  return 1
}

# Convergence: one blocked port breaks the loop, and everyone agrees sw1 is the root.
if wait_alternates 1; then pass "exactly one alternate port in the triangle"; else fail "alternates: $(alternates)"; fi
root=$(root_of sw1)
expect_eq "$(root_of sw2)" "$root" "sw2 agrees sw1 is the root"
expect_eq "$(root_of sw3)" "$root" "sw3 agrees sw1 is the root"
expect_eq "$(cli sw1 show spanning-tree | grep -c 'this bridge is the root')" "1" "sw1 (priority 4096) is the root"

expect_ping h1 h3
expect_ping h1 h5
expect_ping h3 h5
expect_ping h2 h4

# No broadcast storm: with the hosts idle, the switches only exchange BPDUs.
before=$(( $(rx_total sw1) + $(rx_total sw2) + $(rx_total sw3) ))
sleep 3
after=$(( $(rx_total sw1) + $(rx_total sw2) + $(rx_total sw3) ))
if (( after - before < 100 )); then pass "no storm: $((after - before)) packets in 3 s while idle"; else fail "storm: $((after - before)) packets in 3 s"; fi

# Failover under traffic: kill the active sw1-sw2 trunk while h1 pings h3 every 10 ms.
log_file=$(mktemp)
in_host h1 ping -i 0.01 -c 400 -W 1 -q 10.0.10.13 >"$log_file" 2>&1 &
ping_pid=$!
sleep 1
sudo ip link set s1p3 down
wait "$ping_pid" || true
received=$(awk '/packets transmitted/ {print $4}' "$log_file")
lost=$((400 - received))
log "failover lost $lost of 400 pings (10 ms apart)"
if (( lost <= 25 )); then pass "failover took about $((lost * 10)) ms"; else fail "failover lost $lost pings"; fi
rm -f "$log_file"
expect_eq "$(role_of sw2 s2p3)" "root" "sw2 now reaches the root through sw3"
expect_eq "$(alternates)" "0" "no alternate left with the loop broken by the failure"
expect_ping h1 h3

# Recovery: the original tree comes back.
sudo ip link set s1p3 up
if wait_alternates 1; then pass "loop restored: one alternate again"; else fail "alternates after recovery: $(alternates)"; fi
expect_eq "$(role_of sw2 s2p2)" "root" "sw2's root port is the direct trunk again"
expect_ping h1 h3

finish
