# shellcheck shell=bash
# shellcheck disable=SC2034  # definitions consumed by the scripts that source this file
# Shared definition of the 3-switch test topology. Sourced by topo.sh, switches.sh and tests.
# TOPOLOGY=large selects the 8-switch demo topology in scripts/topology-large.sh instead.
#
#            h1 (v10)  h2 (v20)
#               |s1p1    |s1p2
#            +--------------+
#            |     sw1      |   sw1 will own the SVIs (10.0.10.1, 10.0.20.1) in M5
#            +--------------+
#        s1p3 |          | s1p4
#        s2p2 |          | s3p3
#   +-----------+      +-----------+
#   |    sw2    |------|    sw3    |   s2p3 <-> s3p4 closes the loop (RSTP blocks one port)
#   +-----------+      +-----------+
#      | s2p1             | s3p1  | s3p2
#      h3 (v10)           h4(v20) h5 (v10)

CONFIG_DIR=configs
SWITCHES=(sw1 sw2 sw3)
declare -A SWITCH_PORTS=(
  [sw1]="s1p1,s1p2,s1p3,s1p4"
  [sw2]="s2p1,s2p2,s2p3"
  [sw3]="s3p1,s3p2,s3p3,s3p4"
)
# Inter-switch links: "a b" veth pairs, both ends in the root namespace.
TRUNKS=("s1p3 s2p2" "s1p4 s3p3")
LOOP_TRUNK="s2p3 s3p4"
# host -> "switch-port ip/len vlan"
declare -A HOSTS=(
  [h1]="s1p1 10.0.10.11/24 10"
  [h2]="s1p2 10.0.20.22/24 20"
  [h3]="s2p1 10.0.10.13/24 10"
  [h4]="s3p1 10.0.20.24/24 20"
  [h5]="s3p2 10.0.10.15/24 10"
)
if [[ "${TOPOLOGY:-}" == "large" ]]; then
  # shellcheck source=scripts/topology-large.sh
  source "$(dirname "${BASH_SOURCE[0]}")/topology-large.sh"
elif [[ -n "${TOPOLOGY:-}" ]]; then
  echo "unknown TOPOLOGY '${TOPOLOGY}' (use 'large' or leave it unset)" >&2
  exit 2
fi
RUN_DIR=${RUN_DIR:-/tmp/softswitch}
BIN_DIR=${SOFTSWITCH_BIN_DIR:-build/release}

host_port() { echo "${HOSTS[$1]}" | cut -d' ' -f1; }
host_ip() { echo "${HOSTS[$1]}" | cut -d' ' -f2 | cut -d/ -f1; }
host_vlan() { echo "${HOSTS[$1]}" | cut -d' ' -f3; }
