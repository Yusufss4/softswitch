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

}  // namespace
}  // namespace softswitch
