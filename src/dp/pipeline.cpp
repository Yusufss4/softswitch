#include "softswitch/dp/pipeline.hpp"

#include <cstring>

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::dp {

using shm::Decision;
using shm::DropReason;
using shm::StpState;
using shm::Tables;

PortMask Pipeline::forwarding_ports(const Tables& tables) noexcept {
  PortMask mask = 0;
  const auto n = static_cast<PortId>(tables.num_ports < kMaxPorts ? tables.num_ports : kMaxPorts);
  for (PortId p = 0; p < n; ++p) {
    const auto& port = tables.ports[p];
    if (port.up() && port.stp_state == StpState::kForwarding) {
      mask |= port_bit(p);
    }
  }
  return mask;
}

void Pipeline::drop(PortId in_port, DropReason reason) noexcept {
  shm::bump(region_->dp.drops[static_cast<std::size_t>(reason)]);
  if (in_port < kMaxPorts) {
    shm::bump(region_->dp.ports[in_port].drops);
  }
}

void Pipeline::emit_to(const Tables& tables, PortMask ports, VlanId vlan,
                       std::span<const std::uint8_t> frame, Egress& egress) noexcept {
  const auto& entry = tables.vlans[vlan];
  while (ports != 0) {
    const auto p = static_cast<PortId>(__builtin_ctz(ports));
    ports &= ports - 1;
    if (has_port(entry.untagged, p)) {
      egress.emit(p, frame, std::nullopt);
    } else {
      egress.emit(p, frame, pkt::make_tci(vlan));
    }
  }
}

void Pipeline::trace(const Tables& tables, PortId in_port, const Classified& c, Decision decision,
                     DropReason reason, PortMask out_ports, VlanId out_vlan) noexcept {
  if (tables.trace_sample == 0 || (++trace_counter_ % tables.trace_sample) != 0) {
    return;
  }
  shm::TraceRecord* rec = region_->trace.reserve();
  if (rec == nullptr) {
    return;  // lossy by design
  }
  *rec = shm::TraceRecord{};
  rec->in_port = in_port;
  rec->vlan = c.vlan;
  rec->out_vlan = out_vlan;
  rec->decision = decision;
  rec->reason = reason;
  rec->out_ports = out_ports;
  rec->dst = c.eth.dst;
  rec->src = c.eth.src;
  rec->ethertype = c.eth.ethertype;
  rec->len = static_cast<std::uint16_t>(c.untagged.size());
  if (c.eth.ethertype == pkt::kEtherTypeIpv4) {
    if (const auto ip = pkt::parse_ipv4(c.untagged.subspan(pkt::kEthHeaderLen))) {
      rec->ip_proto = ip->protocol;
      rec->src_ip = ip->src;
      rec->dst_ip = ip->dst;
    }
  }
  region_->trace.commit();
}

void Pipeline::process(const Tables& tables, PortId in_port, const RxFrame& frame,
                       Egress& egress) noexcept {
  auto& counters = region_->dp.ports[in_port < kMaxPorts ? in_port : 0];
  shm::bump(counters.rx_packets);
  shm::bump(counters.rx_bytes, frame.data.size());

  if (in_port >= tables.num_ports || !tables.ports[in_port].up()) {
    drop(in_port, DropReason::kPortDown);
    return;
  }

  // ---- classify: find the VLAN and an untagged view of the frame --------------------------
  Classified c;
  auto eth = pkt::parse_ethernet(frame.data);
  if (!eth || frame.data.size() > pkt::kMaxFrameLen + pkt::kVlanTagLen) {
    drop(in_port, DropReason::kMalformed);
    return;
  }
  c.untagged = frame.data;
  std::uint16_t tci = 0;
  if (frame.vlan_tci_valid) {
    c.was_tagged = true;
    tci = frame.vlan_tci;
  } else if (eth->tagged) {
    // In-band tag (no VLAN offload): work on an untagged copy.
    c.was_tagged = true;
    tci = eth->tci;
    const std::size_t len = frame.data.size() - pkt::kVlanTagLen;
    std::memcpy(scratch_.data(), frame.data.data(), pkt::kMacAddrsLen);
    std::memcpy(scratch_.data() + pkt::kMacAddrsLen,
                frame.data.data() + pkt::kMacAddrsLen + pkt::kVlanTagLen, len - pkt::kMacAddrsLen);
    c.untagged = std::span<const std::uint8_t>(scratch_.data(), len);
    eth = pkt::parse_ethernet(c.untagged);
    if (!eth) {
      drop(in_port, DropReason::kMalformed);
      return;
    }
  }
  c.eth = *eth;
  const auto& port = tables.ports[in_port];
  const VlanId tagged_vid = c.was_tagged ? pkt::tci_vid(tci) : VlanId{0};
  c.vlan = tagged_vid != 0 ? tagged_vid : port.pvid;  // VID 0 = priority-tagged
  if (!valid_vlan(c.vlan)) {
    drop(in_port, DropReason::kUntaggedNotAllowed);
    trace(tables, in_port, c, Decision::kDrop, DropReason::kUntaggedNotAllowed, 0, 0);
    return;
  }

  // ---- M1: flood within the VLAN to every other forwarding member -------------------------
  const PortMask out = tables.vlans[c.vlan].members & forwarding_ports(tables) & ~port_bit(in_port);
  if (out == 0) {
    drop(in_port, DropReason::kNoEgress);
    trace(tables, in_port, c, Decision::kDrop, DropReason::kNoEgress, 0, c.vlan);
    return;
  }
  emit_to(tables, out, c.vlan, c.untagged, egress);
  trace(tables, in_port, c, Decision::kFlood, DropReason::kNone, out, c.vlan);
}

void Pipeline::process_inject(const Tables& tables, const shm::PacketMessage& msg,
                              Egress& egress) noexcept {
  shm::bump(region_->dp.injected);
  const auto frame = std::span<const std::uint8_t>(msg.data.data(), msg.len);
  if ((msg.flags & shm::inject_flag::kRaw) != 0) {
    if (msg.port < tables.num_ports && tables.ports[msg.port].up()) {
      egress.emit(msg.port, frame, std::nullopt);
    }
    return;
  }
  if ((msg.flags & shm::inject_flag::kVlan) != 0 && valid_vlan(msg.vlan)) {
    const PortMask out = tables.vlans[msg.vlan].members & forwarding_ports(tables);
    emit_to(tables, out, msg.vlan, frame, egress);
  }
}

}  // namespace softswitch::dp
