#!/usr/bin/env bash
# M5: SVIs answer ARP and ping, across trunks, and learn their neighbours.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
mac_of() { in_host "$1" cat /sys/class/net/eth0/address; }
arp_mac() { cli sw1 show arp | awk -v a="$1" '$1==a {print $2}'; }

expect_eq "$(cli sw1 show ip interface | awk '$1=="vlan10" {print $2, $3}')" "10.0.10.1/24 up" "vlan10 SVI is up"
expect_eq "$(cli sw1 show ip interface | awk '$1=="vlan20" {print $2, $3}')" "10.0.20.1/24 up" "vlan20 SVI is up"

expect_ping h1 10.0.10.1   # directly attached
expect_ping h3 10.0.10.1   # across the sw2 trunk
expect_ping h2 10.0.20.1
expect_ping h4 10.0.20.1   # across the sw3 trunk
expect_ping h1 10.0.20.1   # the other VLAN's SVI, reached through h1's default route

# The hosts resolved the gateway to the router MAC, and sw1 learned them back.
router_mac=$(cli sw1 show ip interface | awk '$1=="vlan10" {print $4}')
expect_eq "$(in_host h1 ip neigh show 10.0.10.1 | awk '{print $5}')" "$router_mac" "h1 resolved its gateway to the router MAC"
expect_eq "$(arp_mac 10.0.10.11)" "$(mac_of h1)" "sw1 learned h1 from its ARP"
expect_eq "$(arp_mac 10.0.10.13)" "$(mac_of h3)" "sw1 learned h3 across the trunk"
expect_eq "$(arp_mac 10.0.20.24)" "$(mac_of h4)" "sw1 learned h4 in VLAN 20"

# Not our address: no answer, nothing learned.
expect_no_ping h1 10.0.10.250
expect_eq "$(arp_mac 10.0.10.250)" "" "unknown addresses are not invented"

cli sw1 clear arp >/dev/null
expect_eq "$(cli sw1 show arp | grep -c '^10\.')" "0" "clear arp empties the cache"
# The router learns from ARP, not from pings: make h1 resolve its gateway again.
in_host h1 ip neigh flush dev eth0
expect_ping h1 10.0.10.1
expect_eq "$(arp_mac 10.0.10.11)" "$(mac_of h1)" "h1 is relearned on the next exchange"

finish
