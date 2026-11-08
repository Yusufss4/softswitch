#!/usr/bin/env bash
# M5: routing between VLAN 10 and VLAN 20 on sw1, across trunks, plus ICMP and static routes.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
drops_for() { cli sw1 show counters | awk -v r="$1" '$1==r {print $2; exit}'; }

expect_eq "$(cli sw1 show ip route | awk '$1=="10.0.20.0/24" {print $2, $4, $5}')" "connected vlan20 active" "connected route for vlan20"

# Inter-VLAN: h1 (VLAN 10) <-> h2 (VLAN 20) on sw1, and to hosts behind the other switches.
expect_ping h1 h2
expect_ping h1 h4   # routed on sw1, then tagged VLAN 20 over the sw3 trunk
expect_ping h3 h2   # in through the sw2 trunk (VLAN 10), out on h2's port
expect_ping h5 h4   # both behind sw3: hairpinned through sw1's router over the trunk

# The routed path rewrites MACs and TTL: h2 sees h1's packets with TTL 63, from the router MAC.
ttl=$(in_host h1 ping -c 1 -W 1 10.0.20.22 | sed -n 's/.*ttl=\([0-9]*\).*/\1/p')
expect_eq "$ttl" "63" "replies cross one router hop (TTL 64 -> 63)"

# TCP across the router (checksum completion + incremental TTL update together).
in_host h2 python3 "$REPO_ROOT/scripts/tcpcheck.py" server 10.0.20.22 6001 &
server=$!
sleep 0.5
echoed=$(in_host h1 python3 "$REPO_ROOT/scripts/tcpcheck.py" client 10.0.20.22 6001 200000 2>/dev/null || true)
wait "$server" || true
expect_eq "$echoed" "200000" "200 KB TCP echo routed between VLANs"

# TTL 1 dies at the router, which says so from its VLAN-10 address.
out=$(in_host h1 ping -c 1 -t 1 -W 1 10.0.20.22 2>&1 || true)
if grep -q "From 10.0.10.1.*Time to live exceeded" <<<"$out"; then
  pass "ICMP time exceeded from 10.0.10.1"
else
  fail "no time exceeded: $out"
fi

# Static route: 192.168.50.0/24 lives behind h4 (10.0.20.24).
in_host h4 ip addr add 192.168.50.1/32 dev lo
expect_no_ping h1 192.168.50.1
if (($(drops_for no-route) > 0)); then pass "sw1 counted no-route drops"; else fail "no no-route drops"; fi
cli sw1 ip route 192.168.50.0/24 10.0.20.24 >/dev/null
expect_eq "$(cli sw1 show ip route | awk '$1=="192.168.50.0/24" {print $2, $3, $5}')" "static 10.0.20.24 active" "static route installed"
expect_ping h1 192.168.50.1

# Turning routing off stops inter-VLAN traffic but not the router's own addresses.
cli sw1 no ip routing >/dev/null
expect_no_ping h1 h2
expect_ping h1 10.0.20.1
cli sw1 ip routing >/dev/null
expect_ping h1 h2

finish
