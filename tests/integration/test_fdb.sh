#!/usr/bin/env bash
# M2: MAC learning, known-unicast forwarding, moves, statics, aging and clearing.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
tx_of() { cli "$1" show counters | awk -v p="$2" '$1==p {print $4; exit}'; }
mac_of() { in_host "$1" cat /sys/class/net/eth0/address; }
# fdb_port SWITCH MAC VLAN -> port name (empty if absent)
fdb_port() { cli "$1" show mac address-table vlan "$3" | awk -v m="$2" '$2==m {print $4; exit}'; }
fdb_type() { cli "$1" show mac address-table vlan "$3" | awk -v m="$2" '$2==m {print $3; exit}'; }

h1mac=$(mac_of h1)
h3mac=$(mac_of h3)

# Learning: one exchange teaches every switch on the path where both hosts are.
expect_ping h1 h3
expect_eq "$(fdb_port sw1 "$h1mac" 10)" "s1p1" "sw1 learned h1 on s1p1"
expect_eq "$(fdb_port sw1 "$h3mac" 10)" "s1p3" "sw1 learned h3 via the sw2 trunk"
expect_eq "$(fdb_port sw2 "$h1mac" 10)" "s2p2" "sw2 learned h1 via the sw1 trunk"
expect_eq "$(fdb_type sw1 "$h1mac" 10)" "dynamic" "learned entries are dynamic"

# Known unicast is not flooded: h1<->h3 traffic never reaches sw3 or h5.
before_trunk=$(tx_of sw1 s1p4)
before_h5=$(tx_of sw3 s3p2)
in_host h1 ping -c 20 -i 0.05 -q 10.0.10.13 >/dev/null
expect_eq "$(tx_of sw1 s1p4)" "$before_trunk" "no h1->h3 unicast leaked onto the sw3 trunk"
expect_eq "$(tx_of sw3 s3p2)" "$before_h5" "h5 received none of the h1<->h3 unicast"

# Station move: h5 takes over h3's MAC and talks; sw1 follows it to the sw3 trunk.
in_host h3 ip link set eth0 down
in_host h5 ip link set eth0 address "$h3mac"
in_host h5 ping -c 2 -i 0.2 -W 1 -q 10.0.10.11 >/dev/null 2>&1 || true
expect_eq "$(fdb_port sw1 "$h3mac" 10)" "s1p4" "sw1 moved h3's MAC to the sw3 trunk"
if cli sw1 show mac address-table | grep -Eq 'moves [1-9]'; then pass "sw1 counted the move"; else fail "no move counted"; fi

# Static entries are configuration and appear in running-config.
cli sw1 mac address-table static 02:00:00:00:99:99 vlan 10 interface s1p1 >/dev/null
expect_eq "$(fdb_type sw1 02:00:00:00:99:99 10)" "static" "static entry installed"
if cli sw1 show running-config | grep -q 'mac address-table static 02:00:00:00:99:99 vlan 10 interface s1p1'; then
  pass "static entry in running-config"
else
  fail "static entry missing from running-config"
fi

# clear removes dynamic entries only.
cli sw1 clear mac address-table >/dev/null
expect_eq "$(fdb_port sw1 "$h1mac" 10)" "" "clear removed h1's dynamic entry"
expect_eq "$(fdb_type sw1 02:00:00:00:99:99 10)" "static" "clear kept the static entry"

# Aging: with a 10 s aging time and silent hosts, sw2 forgets h1.
expect_ping h1 h5
cli sw2 mac address-table aging-time 10 >/dev/null
expect_eq "$(fdb_port sw2 "$h1mac" 10)" "s2p2" "sw2 knows h1 before aging"
sleep 13
expect_eq "$(fdb_port sw2 "$h1mac" 10)" "" "sw2 aged h1 out after 10 s of silence"

finish
