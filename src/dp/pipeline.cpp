#include "softswitch/dp/pipeline.hpp"

#include <algorithm>
#include <cstring>

#include "softswitch/pkt/bpdu.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/igmp.hpp"
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

  // ---- finish offloaded TCP/UDP checksums (works on the private copy) -----------------------
  if (frame.checksum_incomplete && c.eth.ethertype == pkt::kEtherTypeIpv4) {
    if (c.untagged.data() != scratch_.data()) {
      std::memcpy(scratch_.data(), c.untagged.data(), c.untagged.size());
    }
    const std::span<std::uint8_t> mutable_frame(scratch_.data(), c.untagged.size());
    (void)pkt::complete_l4_checksum(mutable_frame);
    c.untagged = mutable_frame;
  }

  // ---- spanning tree BPDUs go to the control plane, whatever the VLAN or STP state ----------
  if (c.eth.dst == pkt::kStpGroupMac) {
    if ((tables.flags & shm::global_flag::kStpEnabled) != 0) {
      punt(tables, in_port, c, shm::PuntReason::kBpdu);
    } else {
      reject(tables, in_port, c, DropReason::kReservedMulticast);
    }
    return;
  }

  // ---- 802.1Q ingress: pick the VLAN, then filter ---------------------------------------
  const auto& port = tables.ports[in_port];
  const VlanId tagged_vid = c.was_tagged ? pkt::tci_vid(tci) : VlanId{0};
  if (tagged_vid != 0) {  // VID 0 is priority-tagged: treated as untagged
    if (port.mode == shm::PortMode::kAccess) {
      reject(tables, in_port, c, DropReason::kTaggedOnAccess);
      return;
    }
    c.vlan = tagged_vid;
  } else {
    if (port.pvid == 0) {
      reject(tables, in_port, c, DropReason::kUntaggedNotAllowed);
      return;
    }
    c.vlan = port.pvid;
  }
  const shm::VlanEntry& vlan = tables.vlans[c.vlan];
  if (!valid_vlan(c.vlan) || (vlan.flags & shm::vlan_flag::kActive) == 0 ||
      !has_port(vlan.members, in_port)) {
    reject(tables, in_port, c, DropReason::kVlanNotMember);
    return;
  }

  // ---- link-local control frames (01:80:c2:00:00:0x) are never forwarded ------------------
  if (is_reserved_multicast(c.eth.dst)) {
    reject(tables, in_port, c, DropReason::kReservedMulticast);
    return;
  }

  // ---- spanning tree: discarding ports neither learn nor forward; learning ports only learn
  if (port.stp_state != StpState::kForwarding && port.stp_state != StpState::kLearning) {
    reject(tables, in_port, c, DropReason::kStpBlocked);
    return;
  }
  learn_source(tables, in_port, c);
  if (port.stp_state == StpState::kLearning) {
    reject(tables, in_port, c, DropReason::kStpBlocked);
    return;
  }

  const PortMask forwarding = vlan.members & forwarding_ports(tables);

  // ---- L3: frames for the router, and ARP the router must see -------------------------------
  if ((vlan.flags & shm::vlan_flag::kHasSvi) != 0) {
    if (c.eth.dst == tables.router_mac) {
      handle_router_frame(tables, in_port, c, egress);
      return;
    }
    if (c.eth.ethertype == pkt::kEtherTypeArp) {
      // Broadcast ARP: the control plane answers for the SVI and learns neighbours from a copy;
      // the hosts on the VLAN still get the original.
      (void)punt(tables, in_port, c, shm::PuntReason::kArp, /*copy_only=*/true);
    }
  }

  // ---- IGMP snooping: multicast only where somebody listens ---------------------------------
  if ((vlan.flags & shm::vlan_flag::kIgmpSnooping) != 0 && c.eth.ethertype == pkt::kEtherTypeIpv4 &&
      c.eth.dst.is_multicast() && !c.eth.dst.is_broadcast() &&
      snoop_multicast(tables, in_port, c, vlan, forwarding, egress)) {
    return;
  }

  // ---- known unicast: one port ------------------------------------------------------------
  if (!c.eth.dst.is_multicast()) {
    if (const shm::FdbSlot* slot = shm::fdb_lookup(tables, c.eth.dst, c.vlan)) {
      if (slot->port == in_port) {
        reject(tables, in_port, c, DropReason::kSameSegment);
        return;
      }
      const PortMask out = port_bit(slot->port) & forwarding;
      if (out != 0) {
        emit_to(tables, out, c.vlan, c.untagged, egress);
        trace(tables, in_port, c, Decision::kForward, DropReason::kNone, out, c.vlan);
        return;
      }
      // The learned port is down or blocked: fall back to flooding until the entry is flushed.
    }
  }

  // ---- unknown unicast, broadcast, multicast: flood within the VLAN -------------------------
  const PortMask out = forwarding & ~port_bit(in_port);
  if (out == 0) {
    reject(tables, in_port, c, DropReason::kNoEgress);
    return;
  }
  emit_to(tables, out, c.vlan, c.untagged, egress);
  trace(tables, in_port, c, Decision::kFlood, DropReason::kNone, out, c.vlan);
}

void Pipeline::learn_source(const Tables& tables, PortId in_port, const Classified& c) noexcept {
  const MacAddress& src = c.eth.src;
  if (src.is_multicast() || src.is_zero()) {
    return;  // never learn group or null addresses
  }
  const shm::FdbSlot* slot = shm::fdb_lookup(tables, src, c.vlan);
  if (slot != nullptr && slot->port == in_port) {
    // Known here: mark it alive for aging. Read first so steady state costs no atomic RMW.
    const std::uint64_t bit = shm::fdb_hash(src, c.vlan) & (shm::kHitBits - 1);
    auto& word = region_->dp.fdb_hits[bit / 64];
    const std::uint64_t mask = std::uint64_t{1} << (bit % 64);
    if ((word.load(std::memory_order_relaxed) & mask) == 0) {
      word.fetch_or(mask, std::memory_order_relaxed);
    }
    return;
  }
  if (slot != nullptr && (slot->flags & shm::slot_flag::kStatic) != 0) {
    return;  // pinned to another port by configuration
  }
  // New or moved. The control plane needs a few ms to publish it, so suppress repeats.
  const std::uint64_t key = (src.to_u64() << 16U) | c.vlan;
  RecentLearn& recent = recent_learns_[mix64(key) & (recent_learns_.size() - 1)];
  if (recent.key == key && recent.port == in_port &&
      now_ns_ - recent.timestamp_ns < kLearnRepeatNs) {
    return;
  }
  if (region_->learn.try_push(shm::LearnEvent{src, c.vlan, in_port})) {
    recent = RecentLearn{key, now_ns_, in_port};
    shm::bump(region_->dp.learn_events);
    notify_ = true;
  }
}

bool Pipeline::punt(const Tables& tables, PortId in_port, const Classified& c,
                    shm::PuntReason reason, bool copy_only) noexcept {
  shm::PacketMessage* msg = region_->punt.reserve();
  if (msg == nullptr || c.untagged.size() > msg->data.size()) {
    if (copy_only) {
      shm::bump(region_->dp.drops[static_cast<std::size_t>(DropReason::kPuntRingFull)]);
    } else {
      reject(tables, in_port, c, DropReason::kPuntRingFull);
    }
    return false;
  }
  msg->port = in_port;
  msg->vlan = c.vlan;
  msg->len = static_cast<std::uint16_t>(c.untagged.size());
  msg->reason = reason;
  msg->flags = 0;
  msg->timestamp_ns = now_ns_;
  std::memcpy(msg->data.data(), c.untagged.data(), c.untagged.size());
  region_->punt.commit();
  shm::bump(region_->dp.punts);
  notify_ = true;
  if (!copy_only) {
    trace(tables, in_port, c, Decision::kPunt, DropReason::kNone, 0, c.vlan);
  }
  return true;
}

bool Pipeline::is_own_address(const Tables& tables, Ipv4Address ip) noexcept {
  const std::size_t n = std::min<std::size_t>(tables.num_svis, shm::kMaxSvis);
  for (std::size_t i = 0; i < n; ++i) {
    if (tables.svis[i].address.address == ip) {
      return true;
    }
  }
  return false;
}

void Pipeline::handle_router_frame(const Tables& tables, PortId in_port, const Classified& c,
                                   Egress& egress) noexcept {
  if (c.eth.ethertype == pkt::kEtherTypeArp) {
    (void)punt(tables, in_port, c, shm::PuntReason::kArp);  // e.g. replies to our requests
    return;
  }
  const auto l3 = c.untagged.subspan(c.eth.payload_offset);
  const auto ip = c.eth.ethertype == pkt::kEtherTypeIpv4 ? pkt::parse_ipv4(l3) : std::nullopt;
  if (!ip || !pkt::ipv4_header_ok(l3)) {
    reject(tables, in_port, c, DropReason::kMalformed);
    return;
  }
  if (is_own_address(tables, ip->dst)) {
    (void)punt(tables, in_port, c, shm::PuntReason::kToRouter);
    return;
  }
  if ((tables.flags & shm::global_flag::kRoutingEnabled) == 0 || ip->dst.is_multicast() ||
      ip->dst.is_broadcast()) {
    reject(tables, in_port, c, DropReason::kNoRoute);
    return;
  }
  if (ip->ttl <= 1) {
    (void)punt(tables, in_port, c, shm::PuntReason::kTtlExpired);  // ICMP time exceeded
    return;
  }
  const shm::RouteEntry* route = shm::route_lookup(tables, ip->dst);
  if (route == nullptr) {
    reject(tables, in_port, c, DropReason::kNoRoute);
    return;
  }
  const Ipv4Address nexthop =
      (route->flags & shm::route_flag::kConnected) != 0 ? ip->dst : route->nexthop;
  const shm::NeighborSlot* neighbor = shm::neighbor_lookup(tables, nexthop);
  if (neighbor == nullptr) {
    (void)punt(tables, in_port, c, shm::PuntReason::kNeighborMiss);  // CP resolves and queues
    return;
  }

  // Rewrite a private copy: next-hop MAC, router MAC, TTL - 1 (incremental checksum).
  if (c.untagged.data() != scratch_.data()) {
    std::memcpy(scratch_.data(), c.untagged.data(), c.untagged.size());
  }
  const std::span<std::uint8_t> frame(scratch_.data(), c.untagged.size());
  neighbor->mac.write_to(frame.data());
  tables.router_mac.write_to(frame.data() + 6);
  (void)pkt::decrement_ttl(frame.subspan(c.eth.payload_offset));

  // Layer 2 in the egress VLAN: the next hop's port if known, else flood that VLAN.
  const VlanId out_vlan = neighbor->vlan;
  const PortMask candidates = tables.vlans[out_vlan].members & forwarding_ports(tables);
  PortMask out = candidates;
  if (const shm::FdbSlot* slot = shm::fdb_lookup(tables, neighbor->mac, out_vlan);
      slot != nullptr && has_port(candidates, slot->port)) {
    out = port_bit(slot->port);
  }
  if (out == 0) {
    reject(tables, in_port, c, DropReason::kNoEgress);
    return;
  }
  emit_to(tables, out, out_vlan, frame, egress);
  trace(tables, in_port, c, Decision::kRouted, DropReason::kNone, out, out_vlan);
}

bool Pipeline::snoop_multicast(const Tables& tables, PortId in_port, const Classified& c,
                               const shm::VlanEntry& vlan, PortMask forwarding,
                               Egress& egress) noexcept {
  const auto l3 = c.untagged.subspan(c.eth.payload_offset);
  const auto ip = pkt::parse_ipv4(l3);
  if (!ip || !ip->dst.is_multicast()) {
    return false;
  }
  const PortMask others = forwarding & ~port_bit(in_port);
  if (ip->protocol == pkt::kIpProtoIgmp && ip->total_len > ip->header_len) {
    // The control plane learns from a copy; the switch decides where the original goes.
    (void)punt(tables, in_port, c, shm::PuntReason::kIgmp, /*copy_only=*/true);
    const bool query = l3[ip->header_len] == pkt::kIgmpQuery;
    // Queries reach every host; reports and leaves only the routers (report suppression).
    const PortMask out = query ? others : (vlan.mrouters & others);
    if (out != 0) {
      emit_to(tables, out, c.vlan, c.untagged, egress);
    }
    trace(tables, in_port, c, query ? Decision::kFlood : Decision::kMulticast, DropReason::kNone,
          out, c.vlan);
    return true;
  }
  if (ip->dst.is_local_multicast()) {
    return false;  // 224.0.0.0/24 (OSPF, mDNS, ...) is always flooded
  }
  const shm::MdbSlot* group = shm::mdb_lookup(tables, ip->dst, c.vlan);
  const PortMask out = ((group != nullptr ? group->ports : PortMask{0}) | vlan.mrouters) & others;
  if (out == 0) {
    reject(tables, in_port, c, DropReason::kNoListeners);  // unregistered: never flooded
    return true;
  }
  emit_to(tables, out, c.vlan, c.untagged, egress);
  trace(tables, in_port, c, Decision::kMulticast, DropReason::kNone, out, c.vlan);
  return true;
}

void Pipeline::reject(const Tables& tables, PortId in_port, const Classified& c,
                      DropReason reason) noexcept {
  drop(in_port, reason);
  trace(tables, in_port, c, Decision::kDrop, reason, 0, c.vlan);
}

bool Pipeline::is_reserved_multicast(const MacAddress& dst) noexcept {
  const auto& b = dst.bytes;
  return b[0] == 0x01 && b[1] == 0x80 && b[2] == 0xC2 && b[3] == 0x00 && b[4] == 0x00 &&
         b[5] <= 0x0F;
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
