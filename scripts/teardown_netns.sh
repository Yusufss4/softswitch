#!/usr/bin/env bash
set -uo pipefail
for ns in $(ip netns list | awk '/^h[0-9]+/{print $1}'); do sudo ip netns del "$ns"; done
for l in $(ip -br link | awk '/^sw-p/{print $1}' | cut -d@ -f1); do sudo ip link del "$l"; done
echo "Torn down"
