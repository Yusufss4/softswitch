#!/usr/bin/env bash
# Creates 3 isolated "hosts" (network namespaces) connected via veth pairs.
# The switch attaches to sw-p1..sw-p3. This keeps experiments off eth0 (your SSH link).
set -euo pipefail
N=${1:-3}
for i in $(seq 1 "$N"); do
  sudo ip netns add h$i 2>/dev/null || true
  sudo ip link add sw-p$i type veth peer name h$i-eth0 2>/dev/null || true
  sudo ip link set h$i-eth0 netns h$i
  sudo ip netns exec h$i ip addr add 10.0.0.$i/24 dev h$i-eth0 2>/dev/null || true
  sudo ip netns exec h$i ip link set h$i-eth0 up
  sudo ip netns exec h$i ip link set lo up
  sudo ip link set sw-p$i up
done
echo "Hosts h1..h$N at 10.0.0.1..$N; switch ports sw-p1..sw-p$N"
