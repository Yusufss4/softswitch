#!/usr/bin/env bash
# M1: three switches in a loop-free line, flood-only forwarding.
source "$(dirname "$0")/lib.sh"
setup

expect_ping h1 h3        # sw1 -> sw2
expect_ping h1 h5        # sw1 -> sw3
expect_ping h3 h5        # sw2 -> sw1 -> sw3 (two switch hops)
expect_ping h2 h4        # other subnet, same flood domain for now

# Every data-plane forwarding thread runs as SCHED_FIFO.
fifo=$(ps -eLo cls,comm | awk '$2=="ss-forward" && $1=="FF"' | wc -l)
expect_eq "$fifo" "3" "three ss-forward threads run as SCHED_FIFO"

# Killing a data plane is noticed by its control plane.
sudo pkill -TERM -f "ss-dataplane --socket $RUN_DIR/sw2.dp.sock"
sleep 0.5
if grep -q "data plane detached" "$RUN_DIR/sw2.cp.log"; then
  pass "sw2 control plane noticed its data plane exit"
else
  fail "sw2 control plane did not notice its data plane exit"
fi
expect_no_ping h1 h3     # sw2 no longer forwards

finish
