#!/usr/bin/env bash
# M2: 802.1Q VLAN separation across access ports and trunks.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
tx_of() { cli "$1" show counters | awk -v p="$2" '$1==p {print $4; exit}'; }
drops_for() { cli "$1" show counters | awk -v r="$2" '$1==r {print $2; exit}'; }

# Reachability inside each VLAN, across trunks.
expect_ping h1 h3
expect_ping h1 h5
expect_ping h2 h4

# Same subnet, different VLAN: h4 (VLAN 20) gets a VLAN-10 address but stays unreachable.
in_host h4 ip addr add 10.0.10.24/24 dev eth0
expect_no_ping h1 10.0.10.24

# Broadcasts stay inside the VLAN: ARP for an unused VLAN-10 address from h1 must reach the
# trunk but never h2's access port (VLAN 20).
before_h2=$(tx_of sw1 s1p2)
before_trunk=$(tx_of sw1 s1p3)
in_host h1 ping -c 3 -i 0.2 -W 1 -q 10.0.10.99 >/dev/null 2>&1 || true
expect_eq "$(tx_of sw1 s1p2)" "$before_h2" "VLAN-10 broadcasts never leave on h2's VLAN-20 port"
if (($(tx_of sw1 s1p3) > before_trunk)); then pass "VLAN-10 broadcasts reach the trunk"; else fail "no broadcasts on the trunk"; fi

# A host can't hop VLANs by tagging its own frames: tagged frames on access ports are dropped.
in_host h1 ip link add link eth0 name eth0.20 type vlan id 20
in_host h1 ip addr add 10.0.20.99/24 dev eth0.20
in_host h1 ip link set eth0.20 up
expect_no_ping h1 10.0.20.22
if (($(drops_for sw1 tagged-on-access) > 0)); then pass "sw1 counted tagged-on-access drops"; else fail "no tagged-on-access drops counted"; fi
in_host h1 ip link del eth0.20

# Pruning VLAN 10 from the sw1-sw2 trunk cuts h1 off from h3 (VLAN 20 keeps working).
cli sw1 interface s1p3 switchport trunk allowed vlan remove 10 >/dev/null
expect_no_ping h1 h3
cli sw1 interface s1p3 switchport trunk allowed vlan add 10 >/dev/null
expect_ping h1 h3

finish
