#include "softswitch/l3/router.hpp"

#include <algorithm>
#include <array>

#include "softswitch/pkt/arp.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/icmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::l3 {
namespace {

/// The IPv4 packet inside an untagged Ethernet frame, if any.
std::optional<std::pair<pkt::Ipv4Header, std::size_t>> ipv4_of(
    std::span<const std::uint8_t> frame) {
  const auto eth = pkt::parse_ethernet(frame);
  if (!eth || eth->tagged || eth->ethertype != pkt::kEtherTypeIpv4) {
    return std::nullopt;
  }
  const auto ip = pkt::parse_ipv4(frame.subspan(eth->payload_offset));
  if (!ip) {
    return std::nullopt;
  }
  return std::pair{*ip, eth->payload_offset};
}

}  // namespace

void Router::set_config(RouterConfig config) {
  config_ = std::move(config);
  const auto before = neighbors_.size();
  std::erase_if(neighbors_, [this](const auto& kv) {
    const Svi* svi = svi_containing(kv.first);
    return svi == nullptr || svi->vlan != kv.second.vlan;
  });
  std::erase_if(pending_,
                [this](const auto& kv) { return svi_in_vlan(kv.second.vlan) == nullptr; });
  neighbors_changed_ = neighbors_changed_ || neighbors_.size() != before;
}

const Svi* Router::svi_in_vlan(VlanId vlan) const noexcept {
  const auto it = std::find_if(config_.svis.begin(), config_.svis.end(),
                               [&](const Svi& s) { return s.vlan == vlan; });
  return it != config_.svis.end() ? &*it : nullptr;
}

const Svi* Router::svi_with_address(Ipv4Address ip) const noexcept {
  const auto it = std::find_if(config_.svis.begin(), config_.svis.end(),
                               [&](const Svi& s) { return s.address.address == ip; });
  return it != config_.svis.end() ? &*it : nullptr;
}

const Svi* Router::svi_containing(Ipv4Address ip) const noexcept {
  const Svi* best = nullptr;
  for (const Svi& s : config_.svis) {
    if (s.address.contains(ip) && (best == nullptr || s.address.length > best->address.length)) {
      best = &s;
    }
  }
  return best;
}

void Router::learn(Ipv4Address ip, const MacAddress& mac, VlanId vlan, Clock::time_point now,
                   std::vector<Output>& out) {
  auto& n = neighbors_[ip];
  if (n.mac != mac || n.vlan != vlan) {
    neighbors_changed_ = true;
  }
  n = Neighbor{mac, vlan, now};
  // Release whatever was waiting for this address.
  if (const auto it = pending_.find(ip); it != pending_.end()) {
    for (auto& frame : it->second.queued) {
      if (auto o = rewrite(std::move(frame), n, config_.mac)) {
        out.push_back(std::move(*o));
      }
    }
    pending_.erase(it);
  }
}

std::vector<Output> Router::on_arp(VlanId vlan, std::span<const std::uint8_t> frame,
                                   Clock::time_point now) {
  std::vector<Output> out;
  const auto eth = pkt::parse_ethernet(frame);
  if (!eth || eth->ethertype != pkt::kEtherTypeArp) {
    return out;
  }
  const auto arp = pkt::parse_arp(frame.subspan(eth->payload_offset));
  const Svi* svi = svi_in_vlan(vlan);
  if (!arp || svi == nullptr || arp->sender_mac.is_multicast() ||
      !svi->address.contains(arp->sender_ip) || arp->sender_ip == svi->address.address) {
    return out;  // not from a station on this SVI's subnet (or someone claiming our address)
  }
  const bool for_us = arp->target_ip == svi->address.address;
  // RFC 826: refresh senders we know; add new ones only when they talk to us.
  if (for_us || neighbors_.contains(arp->sender_ip) || pending_.contains(arp->sender_ip)) {
    learn(arp->sender_ip, arp->sender_mac, vlan, now, out);
  }
  if (for_us && arp->op == pkt::kArpOpRequest) {
    pkt::ArpPacket reply;
    reply.op = pkt::kArpOpReply;
    reply.sender_mac = config_.mac;
    reply.sender_ip = svi->address.address;
    reply.target_mac = arp->sender_mac;
    reply.target_ip = arp->sender_ip;
    Output o{vlan, std::vector<std::uint8_t>(pkt::kMinFrameLen)};
    o.frame.resize(pkt::build_arp_frame(o.frame, arp->sender_mac, config_.mac, reply));
    out.push_back(std::move(o));
  }
  return out;
}

std::vector<Output> Router::on_to_router(VlanId vlan, std::span<const std::uint8_t> frame) const {
  std::vector<Output> out;
  const auto ip = ipv4_of(frame);
  if (!ip || svi_with_address(ip->first.dst) == nullptr ||
      ip->first.protocol != pkt::kIpProtoIcmp) {
    return out;
  }
  Output o{vlan, {frame.begin(), frame.end()}};
  if (pkt::echo_request_to_reply(o.frame)) {
    out.push_back(std::move(o));
  }
  return out;
}

std::vector<Output> Router::on_ttl_expired(VlanId vlan, std::span<const std::uint8_t> frame) const {
  std::vector<Output> out;
  const auto ip = ipv4_of(frame);
  const Svi* svi = svi_in_vlan(vlan);
  const auto eth = pkt::parse_ethernet(frame);
  if (!ip || svi == nullptr || !eth) {
    return out;
  }
  // Never answer an ICMP error with another (RFC 1122 §3.2.2).
  const auto l3 = frame.subspan(ip->second);
  if (ip->first.protocol == pkt::kIpProtoIcmp && l3.size() > ip->first.header_len) {
    const std::uint8_t type = l3[ip->first.header_len];
    if (type != pkt::kIcmpEchoRequest && type != pkt::kIcmpEchoReply) {
      return out;
    }
  }
  Output o{vlan, std::vector<std::uint8_t>(pkt::kEthHeaderLen + 20 + 8 + 60 + 8)};
  const auto len =
      pkt::build_icmp_error_frame(o.frame, eth->src, config_.mac, svi->address.address,
                                  pkt::kIcmpTimeExceeded, 0, l3.first(ip->first.total_len));
  if (len != 0) {
    o.frame.resize(len);
    out.push_back(std::move(o));
  }
  return out;
}

std::optional<std::pair<Ipv4Address, const Svi*>> Router::resolve_nexthop(Ipv4Address dst) const {
  if (const Svi* svi = svi_containing(dst)) {
    return std::pair{dst, svi};
  }
  const StaticRoute* best = nullptr;
  for (const auto& r : config_.static_routes) {
    if (r.prefix.contains(dst) && (best == nullptr || r.prefix.length > best->prefix.length)) {
      best = &r;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  const Svi* svi = svi_containing(best->nexthop);
  if (svi == nullptr) {
    return std::nullopt;
  }
  return std::pair{best->nexthop, svi};
}

std::vector<Output> Router::on_neighbor_miss(VlanId /*vlan*/, std::span<const std::uint8_t> frame,
                                             Clock::time_point now) {
  std::vector<Output> out;
  const auto ip = ipv4_of(frame);
  if (!ip) {
    return out;
  }
  const auto hop = resolve_nexthop(ip->first.dst);
  if (!hop) {
    return out;
  }
  const auto [nexthop, svi] = *hop;
  if (const auto it = neighbors_.find(nexthop); it != neighbors_.end()) {
    // Resolved meanwhile (the data plane's tables were a publish behind): send it on.
    if (auto o = rewrite({frame.begin(), frame.end()}, it->second, config_.mac)) {
      out.push_back(std::move(*o));
    }
    return out;
  }
  auto [it, fresh] = pending_.try_emplace(nexthop);
  Pending& p = it->second;
  p.vlan = svi->vlan;
  if (p.queued.size() < kMaxQueued) {
    p.queued.emplace_back(frame.begin(), frame.end());
  }
  if (fresh) {
    p.tries = 1;
    p.next_try = now + std::chrono::seconds{1};
    if (auto req = arp_request(*svi, nexthop)) {
      out.push_back(std::move(*req));
    }
  }
  return out;
}

std::vector<Output> Router::tick(Clock::time_point now) {
  std::vector<Output> out;
  for (auto it = pending_.begin(); it != pending_.end();) {
    Pending& p = it->second;
    if (now < p.next_try) {
      ++it;
      continue;
    }
    if (p.tries >= kArpRetries) {
      it = pending_.erase(it);  // unreachable: drop what was queued
      continue;
    }
    ++p.tries;
    p.next_try = now + std::chrono::seconds{1};
    if (const Svi* svi = svi_in_vlan(p.vlan)) {
      if (auto req = arp_request(*svi, it->first)) {
        out.push_back(std::move(*req));
      }
    }
    ++it;
  }
  const auto before = neighbors_.size();
  std::erase_if(neighbors_,
                [&](const auto& kv) { return now - kv.second.updated > config_.arp_timeout; });
  neighbors_changed_ = neighbors_changed_ || neighbors_.size() != before;
  return out;
}

std::vector<Route> Router::routes() const {
  std::vector<Route> out;
  out.reserve(config_.svis.size() + config_.static_routes.size());
  for (const Svi& s : config_.svis) {
    out.push_back(Route{Ipv4Prefix{s.address.network(), s.address.length}, true, s.vlan, {}, true});
  }
  for (const auto& r : config_.static_routes) {
    const Svi* svi = svi_containing(r.nexthop);
    out.push_back(Route{Ipv4Prefix{r.prefix.network(), r.prefix.length}, false,
                        svi != nullptr ? svi->vlan : VlanId{0}, r.nexthop, svi != nullptr});
  }
  return out;
}

void Router::clear_neighbors() {
  neighbors_changed_ = neighbors_changed_ || !neighbors_.empty();
  neighbors_.clear();
}

std::optional<Output> Router::arp_request(const Svi& svi, Ipv4Address target) const {
  pkt::ArpPacket req;
  req.op = pkt::kArpOpRequest;
  req.sender_mac = config_.mac;
  req.sender_ip = svi.address.address;
  req.target_ip = target;
  Output o{svi.vlan, std::vector<std::uint8_t>(pkt::kMinFrameLen)};
  o.frame.resize(pkt::build_arp_frame(o.frame, MacAddress::broadcast(), config_.mac, req));
  return o;
}

std::optional<Output> Router::rewrite(std::vector<std::uint8_t> frame, const Neighbor& n,
                                      const MacAddress& router_mac) {
  const auto ip = ipv4_of(frame);
  if (!ip || ip->first.ttl <= 1) {
    return std::nullopt;
  }
  n.mac.write_to(frame.data());
  router_mac.write_to(frame.data() + 6);
  (void)pkt::decrement_ttl(std::span{frame}.subspan(ip->second));
  return Output{n.vlan, std::move(frame)};
}

}  // namespace softswitch::l3
