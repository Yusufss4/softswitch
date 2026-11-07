#!/usr/bin/env bash
# M4: IGMP snooping. Multicast reaches joined hosts only, across trunks, and stops on leave.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }
mcast() { in_host "$1" python3 "$REPO_ROOT/scripts/mcast.py" "${@:2}"; }
seen_by() {  # seen_by HOST SECONDS -> inbound datagrams to 239.1.1.1 captured on the host
  in_host "$1" timeout "$2" tcpdump -i eth0 -Q in -nn -p udp and dst host 239.1.1.1 2>/dev/null \
    | grep -c "239.1.1.1" || true
}
group_ports() { cli "$1" show ip igmp snooping | awk '$2=="239.1.1.1" {print $3}'; }
mrouter_of() { cli "$1" show ip igmp snooping | awk -v v="$2" '$1==v && $2=="on" {print $3}'; }

# sw1 is the querier: the others learn the port towards it as an mrouter port.
deadline=$((SECONDS + 5))
while ((SECONDS < deadline)) && [[ "$(mrouter_of sw2 10)" != "s2p2" ]]; do sleep 0.2; done
expect_eq "$(mrouter_of sw2 10)" "s2p2" "sw2 learned its mrouter port towards the querier"
expect_eq "$(mrouter_of sw3 10)" "s3p3" "sw3 learned its mrouter port towards the querier"

# Before anyone joins, the group is unregistered: h1's stream reaches neither h3 nor h5.
seen_by h5 2 >"$RUN_DIR/h5_before" &
cap=$!
mcast h1 send 239.1.1.1 5000 20 0.05 10.0.10.11 >/dev/null
wait "$cap" || true
expect_eq "$(cat "$RUN_DIR/h5_before")" "0" "unregistered multicast is not flooded to h5"

# h3 joins: the report travels to the querier, and the stream reaches h3 but still not h5.
mcast h3 recv 239.1.1.1 5000 4 10.0.10.13 >"$RUN_DIR/h3_count" &
recv=$!
sleep 0.5
expect_eq "$(group_ports sw2)" "s2p1" "sw2 tracks h3's port as a member"
expect_eq "$(group_ports sw1)" "s1p3" "sw1 learned the member through the sw2 trunk"
expect_eq "$(group_ports sw3)" "" "sw3 knows no members (h5 never joined)"
seen_by h5 2 >"$RUN_DIR/h5_during" &
cap=$!
mcast h1 send 239.1.1.1 5000 40 0.05 10.0.10.11 >/dev/null
wait "$cap" "$recv" || true
received=$(cat "$RUN_DIR/h3_count")
if ((received >= 38)); then pass "h3 received $received of 40 datagrams"; else fail "h3 received only $received of 40"; fi
expect_eq "$(cat "$RUN_DIR/h5_during")" "0" "h5 (same VLAN, not joined) received nothing"

# h3 left when its receiver exited: fast-leave on the host port removes it at once.
sleep 0.3
expect_eq "$(group_ports sw2)" "" "sw2 dropped h3 after the leave (fast-leave on a host port)"
seen_by h3 2 >"$RUN_DIR/h3_after" &
cap=$!
mcast h1 send 239.1.1.1 5000 20 0.05 10.0.10.11 >/dev/null
wait "$cap" || true
expect_eq "$(cat "$RUN_DIR/h3_after")" "0" "after leaving, h3 no longer receives the stream"

finish
