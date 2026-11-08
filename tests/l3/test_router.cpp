#include <gtest/gtest.h>

#include <vector>

#include "softswitch/l3/router.hpp"
#include "softswitch/pkt/arp.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/icmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::l3 {
namespace {

using namespace std::chrono_literals;

const MacAddress kRouterMac = *MacAddress::parse("02:00:00:00:00:fe");
const MacAddress kH1 = *MacAddress::parse("02:00:00:00:00:11");
const MacAddress kH2 = *MacAddress::parse("02:00:00:00:00:22");
Ipv4Address ip(const char* s) { return *Ipv4Address::parse(s); }

RouterConfig two_svis() {
  RouterConfig c;
  c.mac = kRouterMac;
  c.svis = {{10, *Ipv4Prefix::parse("10.0.10.1/24")}, {20, *Ipv4Prefix::parse("10.0.20.1/24")}};
  return c;
}

std::vector<std::uint8_t> arp(std::uint16_t op, const MacAddress& smac, const char* sip,
                              const char* tip, const MacAddress& dst = MacAddress::broadcast()) {
  pkt::ArpPacket a;
  a.op = op;
  a.sender_mac = smac;
  a.sender_ip = ip(sip);
  a.target_ip = ip(tip);
  std::vector<std::uint8_t> f(60);
  f.resize(pkt::build_arp_frame(f, dst, smac, a));
  return f;
}

/// Ethernet+IPv4+ICMP echo request from @p smac/@p sip to the router MAC, @p dip.
std::vector<std::uint8_t> echo(const MacAddress& smac, const char* sip, const char* dip,
                               std::uint8_t ttl = 64) {
  std::vector<std::uint8_t> f(14 + 20 + 8 + 4, 0);
  pkt::write_eth_header(f, kRouterMac, smac, pkt::kEtherTypeIpv4);
  pkt::Ipv4Header h;
  h.total_len = 32;
  h.ttl = ttl;
  h.protocol = pkt::kIpProtoIcmp;
  h.src = ip(sip);
  h.dst = ip(dip);
  pkt::write_ipv4_header(std::span{f}.subspan(14), h);
  f[34] = pkt::kIcmpEchoRequest;
  pkt::store_be16(f.data() + 36, pkt::internet_checksum(std::span{f}.subspan(34)));
  return f;
}

TEST(Router, AnswersArpForItsOwnAddressAndLearnsTheSender) {
  Router r(two_svis());
  const auto out =
      r.on_arp(10, arp(pkt::kArpOpRequest, kH1, "10.0.10.11", "10.0.10.1"), Clock::now());
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].vlan, 10);
  const auto eth = pkt::parse_ethernet(out[0].frame);
  EXPECT_EQ(eth->dst, kH1);
  EXPECT_EQ(eth->src, kRouterMac);
  const auto reply = pkt::parse_arp(std::span{out[0].frame}.subspan(14));
  EXPECT_EQ(reply->op, pkt::kArpOpReply);
  EXPECT_EQ(reply->sender_ip, ip("10.0.10.1"));
  EXPECT_EQ(reply->sender_mac, kRouterMac);
  ASSERT_TRUE(r.neighbors().contains(ip("10.0.10.11")));
  EXPECT_EQ(r.neighbors().at(ip("10.0.10.11")).mac, kH1);
  EXPECT_TRUE(r.take_neighbors_changed());
}

TEST(Router, IgnoresArpForOthersAndFromForeignSubnets) {
  Router r(two_svis());
  EXPECT_TRUE(
      r.on_arp(10, arp(pkt::kArpOpRequest, kH1, "10.0.10.11", "10.0.10.99"), Clock::now()).empty());
  EXPECT_TRUE(r.neighbors().empty());  // only refreshed if already known
  EXPECT_TRUE(
      r.on_arp(10, arp(pkt::kArpOpRequest, kH1, "192.168.1.5", "10.0.10.1"), Clock::now()).empty());
  EXPECT_TRUE(
      r.on_arp(30, arp(pkt::kArpOpRequest, kH1, "10.0.10.11", "10.0.10.1"), Clock::now()).empty());
  EXPECT_TRUE(r.neighbors().empty());
}

TEST(Router, RefreshesKnownNeighborsFromAnyArp) {
  Router r(two_svis());
  const auto t0 = Clock::now();
  (void)r.on_arp(10, arp(pkt::kArpOpRequest, kH1, "10.0.10.11", "10.0.10.1"), t0);
  const auto moved = *MacAddress::parse("02:00:00:00:00:99");
  (void)r.on_arp(10, arp(pkt::kArpOpRequest, moved, "10.0.10.11", "10.0.10.42"),
                 t0 + 1s);  // gratuitous-ish
  EXPECT_EQ(r.neighbors().at(ip("10.0.10.11")).mac, moved);
}

TEST(Router, AnswersPingsToEverySviAddress) {
  Router r(two_svis());
  for (const char* target : {"10.0.10.1", "10.0.20.1"}) {
    const auto out = r.on_to_router(10, echo(kH1, "10.0.10.11", target));
    ASSERT_EQ(out.size(), 1U) << target;
    const auto l3 = std::span{out[0].frame}.subspan(14);
    const auto h = pkt::parse_ipv4(l3);
    EXPECT_EQ(h->src, ip(target));
    EXPECT_EQ(h->dst, ip("10.0.10.11"));
    EXPECT_EQ(pkt::parse_icmp(l3.subspan(20))->type, pkt::kIcmpEchoReply);
  }
  EXPECT_TRUE(r.on_to_router(10, echo(kH1, "10.0.10.11", "10.0.10.77")).empty());
}

TEST(Router, TtlExpiredProducesTimeExceededFromTheIngressSvi) {
  Router r(two_svis());
  const auto out = r.on_ttl_expired(10, echo(kH1, "10.0.10.11", "10.0.20.22", 1));
  ASSERT_EQ(out.size(), 1U);
  const auto l3 = std::span{out[0].frame}.subspan(14);
  const auto h = pkt::parse_ipv4(l3);
  EXPECT_EQ(h->src, ip("10.0.10.1"));
  EXPECT_EQ(h->dst, ip("10.0.10.11"));
  EXPECT_EQ(pkt::parse_icmp(l3.subspan(20))->type, pkt::kIcmpTimeExceeded);
  EXPECT_EQ(pkt::parse_ethernet(out[0].frame)->dst, kH1);
}

TEST(Router, NeighborMissQueuesArpsAndReleasesOnReply) {
  Router r(two_svis());
  const auto t0 = Clock::now();
  const auto pkt1 = echo(kH1, "10.0.10.11", "10.0.20.22");
  auto out = r.on_neighbor_miss(10, pkt1, t0);
  ASSERT_EQ(out.size(), 1U);  // one ARP request into VLAN 20
  EXPECT_EQ(out[0].vlan, 20);
  const auto req = pkt::parse_arp(std::span{out[0].frame}.subspan(14));
  EXPECT_EQ(req->op, pkt::kArpOpRequest);
  EXPECT_EQ(req->target_ip, ip("10.0.20.22"));
  EXPECT_EQ(req->sender_ip, ip("10.0.20.1"));
  EXPECT_TRUE(r.on_neighbor_miss(10, pkt1, t0).empty());  // already resolving: queued, no new ARP
  EXPECT_EQ(r.pending(), 1U);

  out = r.on_arp(20, arp(pkt::kArpOpReply, kH2, "10.0.20.22", "10.0.20.1", kRouterMac), t0 + 10ms);
  ASSERT_EQ(out.size(), 2U);  // both queued packets, rewritten
  for (const auto& o : out) {
    EXPECT_EQ(o.vlan, 20);
    const auto eth = pkt::parse_ethernet(o.frame);
    EXPECT_EQ(eth->dst, kH2);
    EXPECT_EQ(eth->src, kRouterMac);
    const auto l3 = std::span{o.frame}.subspan(14);
    EXPECT_EQ(pkt::parse_ipv4(l3)->ttl, 63);
    EXPECT_TRUE(pkt::ipv4_header_ok(l3));
  }
  EXPECT_EQ(r.pending(), 0U);
}

TEST(Router, UnansweredResolutionRetriesThenGivesUp) {
  Router r(two_svis());
  const auto t0 = Clock::now();
  (void)r.on_neighbor_miss(10, echo(kH1, "10.0.10.11", "10.0.20.99"), t0);
  EXPECT_EQ(r.tick(t0 + 1s).size(), 1U);
  EXPECT_EQ(r.tick(t0 + 2s).size(), 1U);
  EXPECT_TRUE(r.tick(t0 + 3s).empty());
  EXPECT_EQ(r.pending(), 0U);
}

TEST(Router, StaticRoutesResolveThroughConnectedNextHops) {
  auto cfg = two_svis();
  cfg.static_routes = {
      {*Ipv4Prefix::parse("192.168.50.0/24"), ip("10.0.20.24")},
      {*Ipv4Prefix::parse("172.16.0.0/12"), ip("10.9.9.9")}};  // unreachable next hop
  Router r(cfg);
  const auto routes = r.routes();
  ASSERT_EQ(routes.size(), 4U);
  EXPECT_TRUE(routes[0].connected);
  EXPECT_EQ(routes[0].prefix.to_string(), "10.0.10.0/24");
  EXPECT_TRUE(routes[2].active);
  EXPECT_EQ(routes[2].vlan, 20);
  EXPECT_FALSE(routes[3].active);
  // A miss for a destination behind the static route ARPs for the gateway, not the destination.
  const auto out = r.on_neighbor_miss(10, echo(kH1, "10.0.10.11", "192.168.50.1"), Clock::now());
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(pkt::parse_arp(std::span{out[0].frame}.subspan(14))->target_ip, ip("10.0.20.24"));
}

TEST(Router, NeighborsExpireAndReconfigurationDropsForeignOnes) {
  Router r(two_svis());
  const auto t0 = Clock::now();
  (void)r.on_arp(10, arp(pkt::kArpOpRequest, kH1, "10.0.10.11", "10.0.10.1"), t0);
  (void)r.on_arp(20, arp(pkt::kArpOpRequest, kH2, "10.0.20.22", "10.0.20.1"), t0);
  auto cfg = two_svis();
  cfg.svis.pop_back();  // VLAN 20's SVI removed
  r.set_config(cfg);
  EXPECT_EQ(r.neighbors().size(), 1U);
  (void)r.tick(t0 + 301s);
  EXPECT_TRUE(r.neighbors().empty());
}

}  // namespace
}  // namespace softswitch::l3
