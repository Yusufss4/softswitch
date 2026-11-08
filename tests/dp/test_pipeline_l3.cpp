#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/arp.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/icmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch {
namespace {

using shm::DropReason;
using test::PipelineHarness;

const MacAddress kRouterMac = *MacAddress::parse("02:00:00:00:00:fe");
const MacAddress kHost = *MacAddress::parse("02:00:00:00:00:11");

std::vector<std::uint8_t> ipv4_to(const MacAddress& dst_mac, const char* dst_ip,
                                  std::uint8_t ttl = 64) {
  std::vector<std::uint8_t> f(14 + 20 + 8, 0);
  pkt::write_eth_header(f, dst_mac, kHost, pkt::kEtherTypeIpv4);
  pkt::Ipv4Header h;
  h.total_len = 28;
  h.ttl = ttl;
  h.protocol = pkt::kIpProtoIcmp;
  h.src = *Ipv4Address::parse("10.0.1.11");
  h.dst = *Ipv4Address::parse(dst_ip);
  pkt::write_ipv4_header(std::span{f}.subspan(14), h);
  f[34] = pkt::kIcmpEchoRequest;
  pkt::store_be16(f.data() + 36, pkt::internet_checksum(std::span{f}.subspan(34)));
  f.resize(60, 0);
  return f;
}

std::vector<std::uint8_t> arp_frame(const MacAddress& dst) {
  pkt::ArpPacket a;
  a.op = dst.is_broadcast() ? pkt::kArpOpRequest : pkt::kArpOpReply;
  a.sender_mac = kHost;
  a.sender_ip = *Ipv4Address::parse("10.0.1.11");
  a.target_ip = *Ipv4Address::parse("10.0.1.1");
  std::vector<std::uint8_t> f(60);
  f.resize(pkt::build_arp_frame(f, dst, kHost, a));
  return f;
}

class PipelineL3Test : public ::testing::Test {
 protected:
  PipelineHarness h{4};
  void SetUp() override {
    h.tables.router_mac = kRouterMac;
    h.tables.vlans[1].flags |= shm::vlan_flag::kHasSvi;
    h.tables.svis[0] = shm::SviEntry{1, *Ipv4Prefix::parse("10.0.1.1/24")};
    h.tables.num_svis = 1;
  }
  shm::PuntReason punted() {
    shm::PacketMessage m;
    return h.region->punt.try_pop(m) ? m.reason : shm::PuntReason::kNone;
  }
};

TEST_F(PipelineL3Test, BroadcastArpIsPuntedAndStillFlooded) {
  h.receive(0, arp_frame(MacAddress::broadcast()));
  EXPECT_EQ(punted(), shm::PuntReason::kArp);
  EXPECT_EQ(h.egress.ports(), 0b1110U);
}

TEST_F(PipelineL3Test, ArpToTheRouterIsOnlyPunted) {
  h.receive(0, arp_frame(kRouterMac));
  EXPECT_EQ(punted(), shm::PuntReason::kArp);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineL3Test, PingToTheSviIsPuntedToTheRouter) {
  h.receive(0, ipv4_to(kRouterMac, "10.0.1.1"));
  EXPECT_EQ(punted(), shm::PuntReason::kToRouter);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineL3Test, BadIpHeaderToTheRouterIsDropped) {
  auto f = ipv4_to(kRouterMac, "10.0.1.1");
  f[14 + 10] ^= 0xFF;  // corrupt header checksum
  h.receive(0, f);
  EXPECT_EQ(punted(), shm::PuntReason::kNone);
  EXPECT_EQ(h.drops(DropReason::kMalformed), 1U);
}

TEST_F(PipelineL3Test, VlanWithoutSviIgnoresTheRouterMac) {
  h.tables.vlans[1].flags = shm::vlan_flag::kActive;
  h.receive(0, ipv4_to(kRouterMac, "10.0.1.1"));
  EXPECT_EQ(punted(), shm::PuntReason::kNone);
  EXPECT_EQ(h.egress.ports(), 0b1110U);  // just an unknown unicast MAC here
}

/// Two routed VLANs: VLAN 1 (10.0.1.0/24) on p0/p1, VLAN 2 (10.0.2.0/24) on p2 (access)
/// and p3 (trunk, tagged).
class PipelineRoutingTest : public PipelineL3Test {
 protected:
  const MacAddress kDest = *MacAddress::parse("02:00:00:00:00:22");
  void SetUp() override {
    PipelineL3Test::SetUp();
    auto& t = h.tables;
    t.flags |= shm::global_flag::kRoutingEnabled;
    t.vlans[1].members = 0b0011;
    t.vlans[1].untagged = 0b0011;
    t.vlans[2].flags = shm::vlan_flag::kActive | shm::vlan_flag::kHasSvi;
    t.vlans[2].members = 0b1100;
    t.vlans[2].untagged = 0b0100;
    t.svis[1] = shm::SviEntry{2, *Ipv4Prefix::parse("10.0.2.1/24")};
    t.num_svis = 2;
    ASSERT_TRUE(shm::route_insert(
        t, {*Ipv4Prefix::parse("10.0.1.0/24"), shm::route_flag::kConnected, 1, {}}));
    ASSERT_TRUE(shm::route_insert(
        t, {*Ipv4Prefix::parse("10.0.2.0/24"), shm::route_flag::kConnected, 2, {}}));
  }
  void know(const char* ip_text, const MacAddress& mac, VlanId vlan, std::optional<PortId> port) {
    ASSERT_TRUE(shm::neighbor_insert(h.tables, *Ipv4Address::parse(ip_text), mac, vlan));
    if (port) {
      ASSERT_TRUE(shm::fdb_insert(h.tables, mac, vlan, *port, false));
    }
  }
};

TEST_F(PipelineRoutingTest, RoutesToAKnownNextHopAndRewrites) {
  know("10.0.2.22", kDest, 2, PortId{2});
  h.tables.trace_sample = 1;
  h.receive(0, ipv4_to(kRouterMac, "10.0.2.22"));
  ASSERT_EQ(h.egress.sent.size(), 1U);
  const auto& sent = h.egress.sent[0];
  EXPECT_EQ(sent.port, 2);
  EXPECT_FALSE(sent.tci.has_value());  // access port in VLAN 2
  const auto eth = pkt::parse_ethernet(sent.frame);
  EXPECT_EQ(eth->dst, kDest);
  EXPECT_EQ(eth->src, kRouterMac);
  const auto l3 = std::span{sent.frame}.subspan(14);
  EXPECT_EQ(pkt::parse_ipv4(l3)->ttl, 63);
  EXPECT_TRUE(pkt::ipv4_header_ok(l3));  // incremental checksum update is right
  shm::TraceRecord rec;
  ASSERT_TRUE(h.region->trace.try_pop(rec));
  EXPECT_EQ(rec.decision, shm::Decision::kRouted);
  EXPECT_EQ(rec.vlan, 1);
  EXPECT_EQ(rec.out_vlan, 2);
}

TEST_F(PipelineRoutingTest, UnknownPortFloodsTheEgressVlanWithItsTagging) {
  know("10.0.2.22", kDest, 2, std::nullopt);
  h.receive(0, ipv4_to(kRouterMac, "10.0.2.22"));
  EXPECT_EQ(h.egress.ports(), 0b1100U);
  for (const auto& s : h.egress.sent) {
    if (s.port == 3) {
      ASSERT_TRUE(s.tci.has_value());
      EXPECT_EQ(pkt::tci_vid(*s.tci), 2);  // tagged on the trunk with the egress VLAN
    }
  }
}

TEST_F(PipelineRoutingTest, NeighborMissIsPuntedForResolution) {
  h.receive(0, ipv4_to(kRouterMac, "10.0.2.99"));
  EXPECT_EQ(punted(), shm::PuntReason::kNeighborMiss);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineRoutingTest, ExpiringTtlIsPuntedForIcmp) {
  know("10.0.2.22", kDest, 2, PortId{2});
  h.receive(0, ipv4_to(kRouterMac, "10.0.2.22", 1));
  EXPECT_EQ(punted(), shm::PuntReason::kTtlExpired);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineRoutingTest, NoRouteIsDropped) {
  h.receive(0, ipv4_to(kRouterMac, "172.16.0.1"));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kNoRoute), 1U);
}

TEST_F(PipelineRoutingTest, RoutingDisabledDropsInsteadOfRouting) {
  know("10.0.2.22", kDest, 2, PortId{2});
  h.tables.flags = 0;
  h.receive(0, ipv4_to(kRouterMac, "10.0.2.22"));
  EXPECT_EQ(h.drops(DropReason::kNoRoute), 1U);
}

TEST_F(PipelineRoutingTest, StaticRouteGoesThroughItsGateway) {
  ASSERT_TRUE(shm::route_insert(
      h.tables, {*Ipv4Prefix::parse("192.168.50.0/24"), 0, 2, *Ipv4Address::parse("10.0.2.24")}));
  const auto gw = *MacAddress::parse("02:00:00:00:00:24");
  know("10.0.2.24", gw, 2, PortId{3});
  h.receive(0, ipv4_to(kRouterMac, "192.168.50.1"));
  ASSERT_EQ(h.egress.sent.size(), 1U);
  EXPECT_EQ(h.egress.sent[0].port, 3);
  EXPECT_EQ(pkt::parse_ethernet(h.egress.sent[0].frame)->dst, gw);
}

}  // namespace
}  // namespace softswitch
