#!/usr/bin/env bash
# The 8-switch ring with two chords (TOPOLOGY=large): RSTP has three loops to break, two data
# planes share each CPU, and a two-step failure must reroute every flow within a second.
#
# This topology found two bugs the 3-switch tests could not: a data plane spinning on a
# socket error after a link flap (starving the other data plane on its CPU, whose bridge then
# declared itself root), and topology changes ignored inside the 2 x hello window (MACs left
# pointing into a dead end until the next change).
export TOPOLOGY=large
source "$(dirname "$0")/lib.sh"
TOPO_LOOP=1 setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
# Trunk ports (not edge) that are not forwarding, over all switches: one per blocked link.
blocked_ports() {
  for sw in "${SWITCHES[@]}"; do
    cli "$sw" show spanning-tree | awk '$1 ~ /^s[0-9]p/ && $6 == "no" && $3 != "forwarding"'
  done | wc -l
}
roots() {
  for sw in "${SWITCHES[@]}"; do
    cli "$sw" show spanning-tree | awk '/^Root ID/ {print $3}'
  done | sort -u
}

for _ in $(seq 50); do (($(blocked_ports) == 3)) && break; sleep 0.2; done
expect_eq "$(blocked_ports)" 3 "RSTP blocks exactly the 3 redundant links (10 links, 8 bridges)"
expect_eq "$(roots)" "4096.02:39:23:9b:a1:1c" "every bridge agrees that sw1 is the root"

expect_ping h3 h5  # VLAN 10, sw2 -> sw4
expect_ping h4 h8  # VLAN 20, sw3 -> sw7
expect_ping h9 h6  # routed 10 -> 20 by sw1, sw8 -> sw5
expect_ping h7 h2  # routed 10 -> 20, sw6 -> sw1

# Two failures, then recovery, under traffic. Each flow sends 10 pings a second.
flows=("h3 h5" "h4 h8" "h9 h6" "h7 h2")
for f in "${flows[@]}"; do
  read -r src dst <<<"$f"
  in_host "$src" ping -q -i 0.1 -W 0.5 -w 14 "$(host_ip "$dst")" >"$RUN_DIR/large-$src.ping" 2>&1 &
done
sleep 3
sudo ip link set s1p5 down  # the sw1-sw5 chord
sleep 3
sudo ip link set s1p4 down  # sw1-sw8 as well
sleep 3
sudo ip link set s1p5 up
sudo ip link set s1p4 up
wait
for f in "${flows[@]}"; do
  read -r src dst <<<"$f"
  read -r sent got < <(awk '/packets transmitted/ {print $1, $4}' "$RUN_DIR/large-$src.ping")
  lost=$((sent - got))
  # Sub-second reconvergence twice costs at most a few pings at 10 per second.
  if ((sent > 100 && lost <= 6)); then
    pass "$src -> $dst survived both failures and the recovery ($lost of $sent lost)"
  else
    fail "$src -> $dst lost $lost of $sent pings across the failures"
  fi
done

for _ in $(seq 50); do (($(blocked_ports) == 3)) && break; sleep 0.2; done
expect_eq "$(blocked_ports)" 3 "the original tree is back after recovery"

# Every data plane sleeps again: none is spinning on a socket error left by the flaps.
for sw in "${SWITCHES[@]}"; do
  busy=$(forward_busy "$sw")
  if ((busy >= 0 && busy < 20)); then
    pass "$sw's forwarding thread is idle (${busy}% CPU)"
  else
    fail "$sw's forwarding thread is busy without traffic (${busy}% CPU)"
  fi
done
finish
