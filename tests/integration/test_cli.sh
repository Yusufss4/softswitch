#!/usr/bin/env bash
# M2: CLI socket, startup config and runtime reconfiguration.
source "$(dirname "$0")/lib.sh"
setup

cli() { "$BIN_DIR/sscli" --run-dir "$RUN_DIR" -s "$@"; }

# The startup config was applied.
vlans=$(cli sw1 show vlan)
if grep -Eq '^10 +users +s1p1 +s1p3,s1p4$' <<<"$vlans"; then
  pass "sw1 VLAN 10 has s1p1 untagged and both trunks tagged"
else
  fail "unexpected 'show vlan' on sw1:"$'\n'"$vlans"
fi
expect_eq "$(cli sw2 show running-config | grep -c 'switchport mode trunk')" "2" "sw2 running-config lists two trunks"

# Errors come back with a % prefix and a failing exit status.
if out=$(cli sw1 interface nope shutdown); then
  fail "invalid interface was accepted"
else
  if [[ "$out" == "% no such interface 'nope'"* ]]; then
    pass "invalid command rejected: $out"
  else
    fail "bad error text: $out"
  fi
fi

# Runtime change: shutting the trunk to sw2 cuts h1 off from h3, and undoing it restores it.
expect_ping h1 h3
cli sw1 interface s1p3 shutdown >/dev/null
expect_no_ping h1 h3
cli sw1 interface s1p3 no shutdown >/dev/null
expect_ping h1 h3

# running-config replays: feed it back through the CLI and nothing changes.
before=$(cli sw3 show running-config)
grep -v '^!' <<<"$before" | while read -r line; do cli sw3 "$line" >/dev/null; done
expect_eq "$(cli sw3 show running-config)" "$before" "sw3 running-config is idempotent when replayed"

# Counters and data-plane state are visible.
if cli sw1 show dataplane | grep -q '^attached: *yes'; then pass "show dataplane reports attached"; else fail "show dataplane"; fi
if cli sw1 show interfaces | grep -Eq '^s1p1 +up +up +access +10 '; then pass "show interfaces"; else fail "show interfaces: $(cli sw1 show interfaces)"; fi

finish
