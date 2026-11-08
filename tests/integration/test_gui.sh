#!/usr/bin/env bash
# M6: the read-only web GUI: static page, JSON state, live trace stream.
source "$(dirname "$0")/lib.sh"
setup

base=http://127.0.0.1:8081
status() { curl -s -o /dev/null -w "%{http_code}" "$@"; }

expect_eq "$(status "$base/")" "200" "GET / serves the page"
expect_eq "$(curl -s -o /dev/null -w '%{content_type}' "$base/app.js")" "text/javascript; charset=utf-8" "app.js is served as JavaScript"
expect_eq "$(status "$base/nope")" "404" "unknown paths are 404"
expect_eq "$(status -X POST "$base/api/state")" "405" "the GUI is read-only (POST refused)"
expect_eq "$(curl -s -D - -o /dev/null "$base/api/state" | grep -ci '^access-control-allow-origin: \*')" "1" "CORS lets one page show every switch"

expect_ping h1 h2
state=$(curl -s "$base/api/state")
summary=$(python3 -c '
import json, sys
s = json.loads(sys.argv[1])
print(s["name"], len(s["ports"]), s["stp"]["is_root"], any(r["prefix"] == "10.0.20.0/24" for r in s["routes"]),
      any(a["ip"] == "10.0.10.11" for a in s["arp"]))' "$state")
expect_eq "$summary" "sw1 4 True True True" "/api/state has ports, STP, routes and ARP"

# Live stream: a routed ping shows up as a 'routed' trace event within a second.
timeout 3 curl -s -N "$base/events" >"$RUN_DIR/sse.txt" &
sse=$!
sleep 0.5
in_host h1 ping -c 2 -i 0.2 -q 10.0.20.22 >/dev/null
wait "$sse" || true
if grep -q '"decision":"routed"' "$RUN_DIR/sse.txt"; then pass "SSE stream carried a routed trace event"; else fail "no routed event on /events"; fi
expect_eq "$(head -c 6 "$RUN_DIR/sse.txt")" "retry:" "SSE stream starts with a retry hint"

traces=$(curl -s "$base/api/traces" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)) > 0)')
expect_eq "$traces" "True" "/api/traces keeps recent decisions"
expect_eq "$(status http://127.0.0.1:8082/api/state)" "200" "sw2 serves its own API"

finish
