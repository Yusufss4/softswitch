# shellcheck shell=bash
# shellcheck disable=SC2024  # output files are redirected by the user's shell on purpose, so they stay user-owned
# Helpers shared by scripts/bench.sh and scripts/perf.sh: a one-switch topology (generator and
# sink namespaces on veth pairs), switch start/stop for softswitch and the kernel bridge, and
# measurements. Source it from the repository root.
#
# CPU placement: control plane cpu 0, generator cpu 1, sink cpu 2, data plane cpu 3.
BIN=${SOFTSWITCH_BIN_DIR:-build/release}
OUT=${OUT:-reports/bench}
RUN=/tmp/softswitch-bench
DURATION=${DURATION:-5}
RESULTS="$OUT/results.jsonl"
mkdir -p "$OUT" "$RUN"

cleanup() {
  sudo pkill -TERM -x ss-dataplane 2>/dev/null || true
  pkill -TERM -f "softswitchd --name bench" 2>/dev/null || true
  sudo ip link del brbench 2>/dev/null || true
  sudo ip netns del bgen 2>/dev/null || true
  sudo ip netns del bsink 2>/dev/null || true
  sudo ip link del b1p1 2>/dev/null || true
  sudo ip link del b1p2 2>/dev/null || true
  sleep 0.3
}
topology() {
  cleanup
  for pair in "bgen b1p1" "bsink b1p2"; do
    read -r ns port <<<"$pair"
    sudo ip netns add "$ns"
    sudo ip link add "$port" type veth peer name eth0 netns "$ns"
    sudo ip netns exec "$ns" ip link set lo up
    sudo ip netns exec "$ns" ip link set eth0 up
    sudo ip netns exec "$ns" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
    sudo sysctl -qw "net.ipv6.conf.$port.disable_ipv6=1"
    sudo ip link set "$port" up
  done
  # Packet sockets see frames before tc ingress, so ss-sink still counts them, but the sink's
  # IP stack never runs. veth delivers synchronously, so without this the sink's IP input
  # (routing lookup, flow dissection) would be billed to the switch's forwarding CPU.
  sudo ip netns exec bsink tc qdisc add dev eth0 clsact
  sudo ip netns exec bsink tc filter add dev eth0 ingress matchall action drop
  SINK_MAC=$(sudo ip netns exec bsink cat /sys/class/net/eth0/address)
}

# start_softswitch TX_MODE; extra softswitchd flags may be passed in CP_EXTRA_ARGS.
start_softswitch() {
  local extra=()
  read -r -a extra <<<"${CP_EXTRA_ARGS:-}"
  cat >"$RUN/bench.conf" <<CONF
hostname bench
no spanning-tree
no ip igmp snooping
interface b1p1 spanning-tree portfast
interface b1p2 spanning-tree portfast
mac address-table static $SINK_MAC vlan 1 interface b1p2
CONF
  taskset -c 0 "$BIN/softswitchd" --name bench --ports b1p1,b1p2 --run-dir "$RUN" --config "$RUN/bench.conf" \
    "${extra[@]}" >"$RUN/cp.log" 2>&1 &
  for _ in $(seq 50); do [[ -S "$RUN/bench.dp.sock" ]] && break; sleep 0.1; done
  sudo "$BIN/ss-dataplane" --socket "$RUN/bench.dp.sock" --name bench --cpu 3 --tx-mode "$1" >"$RUN/dp.log" 2>&1 &
  for _ in $(seq 50); do grep -q "data plane attached" "$RUN/cp.log" && break; sleep 0.1; done
  sleep 0.5
}

stop_softswitch() {
  sudo pkill -TERM -x ss-dataplane 2>/dev/null || true
  pkill -TERM -f "softswitchd --name bench" 2>/dev/null || true
  sleep 0.5
}

start_bridge() {
  sudo ip link add brbench type bridge stp_state 0 forward_delay 0
  sudo ip link set b1p1 master brbench
  sudo ip link set b1p2 master brbench
  sudo ip link set brbench up
  sleep 0.5
}

stop_bridge() { sudo ip link del brbench 2>/dev/null || true; sleep 0.3; }

# Prints {"switch_ring_drops": N, "switch_tx_drops": M}: frames the switch lost in its ingress
# RX ring and on egress. Zeros when no softswitch is running (the Linux bridge baseline).
switch_counters() {
  if [[ -S "$RUN/bench.cli.sock" ]]; then
    "$BIN/sscli" -s bench --run-dir "$RUN" show counters |
      awk '$1=="b1p1"{r=$7} $1=="b1p2"{t=$6} END{printf "{\"switch_ring_drops\":%d,\"switch_tx_drops\":%d}\n", r, t}'
  else
    echo '{"switch_ring_drops":0,"switch_tx_drops":0}'
  fi
}

# measure LABEL SIZE RATE [HISTOGRAM]
measure() {
  local label=$1 size=$2 rate=$3 hist=${4:-}
  local before after
  sudo ip netns exec bsink "$BIN/ss-sink" --iface eth0 --cpu 2 --duration "$(awk "BEGIN{print $DURATION + 1.5}")" \
    >"$RUN/sink.json" &
  local sink=$!
  sleep 0.5
  before=$(switch_counters)
  local gen
  gen=$(sudo ip netns exec bgen "$BIN/ss-pktgen" --iface eth0 --cpu 1 --dst-mac "$SINK_MAC" --size "$size" \
    --rate "$rate" --duration "$DURATION")
  wait "$sink"
  sleep 0.2  # the data plane samples the kernel drop counters every 100 ms
  after=$(switch_counters)
  local line
  line=$(python3 -c '
import json, sys
g, s, b, a = (json.loads(x) for x in sys.argv[1:5])
hist = s.pop("hist", [])
if sys.argv[8]:  # latency histogram CSV requested
    with open(sys.argv[8], "w") as f:
        f.write("upper_us,count\n" + "".join(f"{u:g},{c}\n" for u, c in hist))
print(json.dumps({"label": sys.argv[5], "size": int(sys.argv[6]), "rate": int(sys.argv[7]),
                  "gen": g, "sink": s, "switch": {k: a[k] - b[k] for k in a}}))
' "$gen" "$(cat "$RUN/sink.json")" "$before" "$after" "$label" "$size" "$rate" "${hist:+$OUT/$hist}")
  echo "$line" | tee -a "$RESULTS"
}

# ndr LABEL SIZE: RFC 2544-style search for the no-drop rate, the highest offered rate with
# loss below 0.01 %. A trial where the generator cannot reach the target rate counts as a
# failure: the sender is then the bottleneck (as with the kernel bridge on veth, which forwards
# inside the sender's transmit call), and the best rate seen is what gets reported.
ndr() {
  local label=$1 size=$2 lo=50000 hi=1200000 best=0 mid
  local saved_duration=$DURATION saved_results=$RESULTS
  DURATION=${NDR_DURATION:-2}
  RESULTS="$RUN/trials.jsonl"
  for _ in $(seq "${NDR_STEPS:-8}"); do
    mid=$(((lo + hi) / 2))
    measure "$label" "$size" "$mid" >/dev/null
    read -r ok fwd < <(python3 -c '
import json, sys
r = json.loads(open(sys.argv[1]).read().splitlines()[-1])
sent, got = r["gen"]["sent"], r["sink"]["received"]
ok = sent > 0 and (sent - got) / sent < 1e-4 and r["gen"]["pps"] >= 0.98 * r["rate"]
print(int(ok), r["sink"]["pps"])
' "$RESULTS")
    if [[ "$ok" == 1 ]]; then
      lo=$mid
      ((fwd > best)) && best=$fwd
    else
      hi=$mid
    fi
  done
  DURATION=$saved_duration
  RESULTS=$saved_results
  echo "{\"label\": \"$label\", \"size\": $size, \"ndr_pps\": $best}" | tee -a "$OUT/ndr.jsonl"
}
