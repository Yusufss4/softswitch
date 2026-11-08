# shellcheck shell=bash
# shellcheck disable=SC2034  # definitions consumed by the scripts that source this file
# The 8-switch demo topology (TOPOLOGY=large): a ring sw1..sw8 with two chords, sw1-sw5 and
# sw3-sw7, so RSTP has several loops to break. sw1 is the root and routes between VLANs 10
# and 20. Configs live in configs/large/.
#
#                 h1 h2
#                  sw1 ─────────── sw2 ── h3
#             sw8 ╱ │                  ╲
#        h9 ──   ╱  │ chord             sw3 ── h4
#               │   │                    │ ╲
#              sw7 ─┼──────── chord ─────┘  sw4 ── h5
#        h8 ──  ╲   │                      ╱
#               sw6 ─────────── sw5 ──────
#               h7               h6
#
# Ports: sNp1 is the host port; sNp2.. are trunks (sw1 also has p2 for h2).

SWITCHES=(sw1 sw2 sw3 sw4 sw5 sw6 sw7 sw8)
declare -A SWITCH_PORTS=(
  [sw1]="s1p1,s1p2,s1p3,s1p4,s1p5"
  [sw2]="s2p1,s2p2,s2p3"
  [sw3]="s3p1,s3p2,s3p3,s3p4"
  [sw4]="s4p1,s4p2,s4p3"
  [sw5]="s5p1,s5p2,s5p3,s5p4"
  [sw6]="s6p1,s6p2,s6p3"
  [sw7]="s7p1,s7p2,s7p3,s7p4"
  [sw8]="s8p1,s8p2,s8p3"
)
# Ring sw1-sw2-...-sw8-sw1, then the sw1-sw5 chord.
TRUNKS=("s1p3 s2p2" "s2p3 s3p2" "s3p3 s4p2" "s4p3 s5p2" "s5p3 s6p2" "s6p3 s7p2" "s7p3 s8p2"
  "s8p3 s1p4" "s1p5 s5p4")
# The sw3-sw7 chord; like the 3-switch topology, `topo.sh up` leaves it down without --loop.
LOOP_TRUNK="s3p4 s7p4"
# host -> "switch-port ip/len vlan"
declare -A HOSTS=(
  [h1]="s1p1 10.0.10.11/24 10"
  [h2]="s1p2 10.0.20.22/24 20"
  [h3]="s2p1 10.0.10.13/24 10"
  [h4]="s3p1 10.0.20.24/24 20"
  [h5]="s4p1 10.0.10.15/24 10"
  [h6]="s5p1 10.0.20.26/24 20"
  [h7]="s6p1 10.0.10.17/24 10"
  [h8]="s7p1 10.0.20.28/24 20"
  [h9]="s8p1 10.0.10.19/24 10"
)
CONFIG_DIR=configs/large
