#!/usr/bin/env bash
# M3: rtnetlink link monitoring. Taking a trunk down is seen at once on both ends, flushes the
# MACs learned through it, and bringing it back restores forwarding.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
oper_of() { cli "$1" show interfaces | awk -v p="$2" '$1==p {print $3; exit}'; }
macs_on() { cli "$1" show mac address-table interface "$2" | awk '$3=="dynamic"' | wc -l; }

expect_ping h1 h3
expect_eq "$(oper_of sw1 s1p3)" "up" "sw1 s1p3 starts up"
if (($(macs_on sw1 s1p3) > 0)); then pass "sw1 learned MACs behind s1p3"; else fail "nothing learned on s1p3"; fi

sudo ip link set s1p3 down
sleep 0.5
expect_eq "$(oper_of sw1 s1p3)" "down" "sw1 sees s1p3 admin-down"
expect_eq "$(oper_of sw2 s2p2)" "down" "sw2 sees carrier loss on the other end (s2p2)"
expect_eq "$(macs_on sw1 s1p3)" "0" "sw1 flushed the MACs learned on s1p3"
expect_no_ping h1 h3
if grep -q "link s1p3 is down" "$RUN_DIR/sw1.cp.log"; then pass "sw1 logged the link change"; else fail "no link log"; fi

sudo ip link set s1p3 up
sleep 0.5
expect_eq "$(oper_of sw2 s2p2)" "up" "sw2 sees carrier back"
expect_ping h1 h3

finish
