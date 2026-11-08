#!/usr/bin/env bash
# Creates or removes the veth/netns test topology (see scripts/topology.sh; TOPOLOGY=large for 8
# switches).
# Usage: scripts/topo.sh up [--loop] | down
# Without --loop the LOOP_TRUNK link stays down (sw2-sw3, or sw3-sw7 in the large topology).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/topology.sh

link_up() {
  sudo ip link set "$1" up
  sudo sysctl -qw "net.ipv6.conf.$1.disable_ipv6=1" || true  # keep the switch ports quiet
}

up() {
  local loop=${1:-}
  for pair in "${TRUNKS[@]}" "$LOOP_TRUNK"; do
    read -r a b <<<"$pair"
    sudo ip link add "$a" type veth peer name "$b"
    link_up "$a"
    link_up "$b"
  done
  if [[ "$loop" != "--loop" ]]; then
    read -r a b <<<"$LOOP_TRUNK"
    sudo ip link set "$a" down
  fi
  for h in "${!HOSTS[@]}"; do
    local port ip
    port=$(host_port "$h")
    ip=$(echo "${HOSTS[$h]}" | cut -d' ' -f2)
    sudo ip netns add "$h"
    sudo ip link add "$port" type veth peer name eth0 netns "$h"
    link_up "$port"
    sudo ip netns exec "$h" ip link set lo up
    sudo ip netns exec "$h" ip addr add "$ip" dev eth0
    sudo ip netns exec "$h" ip link set eth0 up
    # Default route via the subnet's .1 (sw1's SVI for that VLAN).
    sudo ip netns exec "$h" ip route add default via "${ip%.*}.1"
    sudo ip netns exec "$h" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
  done
  echo "topology up (loop: ${loop:-off})"
}

down() {
  for h in "${!HOSTS[@]}"; do
    sudo ip netns del "$h" 2>/dev/null || true
  done
  for pair in "${TRUNKS[@]}" "$LOOP_TRUNK"; do
    read -r a _ <<<"$pair"
    sudo ip link del "$a" 2>/dev/null || true
  done
  for h in "${!HOSTS[@]}"; do
    sudo ip link del "$(host_port "$h")" 2>/dev/null || true
  done
  echo "topology down"
}

case "${1:-}" in
  up) up "${2:-}" ;;
  down) down ;;
  *) echo "usage: $0 up [--loop] | down" >&2; exit 2 ;;
esac
