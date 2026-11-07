#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/igmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch {
namespace {

using shm::DropReason;
using test::FrameBuilder;
using test::PipelineHarness;

/// Ethernet + IPv4 frame to @p dst_ip with the given protocol and payload.
std::vector<std::uint8_t> ip_frame(const char* dst_ip, std::uint8_t proto,
                                   std::vector<std::uint8_t> payload) {
  const auto dst = *Ipv4Address::parse(dst_ip);
  std::vector<std::uint8_t> f(14 + 20 + payload.size(), 0);
  multicast_mac(dst).write_to(f.data());
  MacAddress::parse("02:00:00:00:00:01")->write_to(f.data() + 6);
  pkt::store_be16(f.data() + 12, pkt::kEtherTypeIpv4);
  pkt::Ipv4Header h;
  h.total_len = static_cast<std::uint16_t>(20 + payload.size());
  h.ttl = 1;
  h.protocol = proto;
  h.src = *Ipv4Address::parse("10.0.10.11");
  h.dst = dst;
  pkt::write_ipv4_header(std::span{f}.subspan(14), h);
  std::copy(payload.begin(), payload.end(), f.begin() + 34);
  if (f.size() < 60) {
    f.resize(60, 0);
  }
  return f;
}

std::vector<std::uint8_t> igmp(std::uint8_t type, const char* group) {
  std::vector<std::uint8_t> m(8, 0);
  m[0] = type;
  pkt::store_be32(m.data() + 4, Ipv4Address::parse(group)->value);
  pkt::store_be16(m.data() + 2, pkt::internet_checksum(m));
  return m;
}

class PipelineIgmpTest : public ::testing::Test {
 protected:
  PipelineHarness h{4};
  void SetUp() override { h.tables.vlans[1].flags |= shm::vlan_flag::kIgmpSnooping; }
  std::size_t punts() {
    std::size_t n = 0;
    shm::PacketMessage m;
    while (h.region->punt.try_pop(m)) {
      EXPECT_EQ(m.reason, shm::PuntReason::kIgmp);
      ++n;
    }
    return n;
  }
};

TEST_F(PipelineIgmpTest, UnregisteredMulticastIsNotFlooded) {
  h.receive(0, ip_frame("239.1.1.1", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kNoListeners), 1U);
}

TEST_F(PipelineIgmpTest, UnregisteredMulticastGoesToMrouterPorts) {
  h.tables.vlans[1].mrouters = 0b1000;
  h.receive(0, ip_frame("239.1.1.1", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_EQ(h.egress.ports(), 0b1000U);
}

TEST_F(PipelineIgmpTest, RegisteredGroupGoesToMembersAndMrouters) {
  h.tables.vlans[1].mrouters = 0b1000;
  ASSERT_TRUE(shm::mdb_insert(h.tables, *Ipv4Address::parse("239.1.1.1"), 1, 0b0101));
  h.receive(0, ip_frame("239.1.1.1", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_EQ(h.egress.ports(), 0b1100U);  // member p2 + mrouter p3, never the ingress p0
}

TEST_F(PipelineIgmpTest, OtherGroupsStayUnregistered) {
  ASSERT_TRUE(shm::mdb_insert(h.tables, *Ipv4Address::parse("239.1.1.1"), 1, 0b0100));
  h.receive(0, ip_frame("239.9.9.9", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineIgmpTest, LinkLocalMulticastIsAlwaysFlooded) {
  h.receive(0, ip_frame("224.0.0.251", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_EQ(h.egress.ports(), 0b1110U);
}

TEST_F(PipelineIgmpTest, WithoutSnoopingMulticastFloods) {
  h.tables.vlans[1].flags = shm::vlan_flag::kActive;
  h.receive(0, ip_frame("239.1.1.1", pkt::kIpProtoUdp, std::vector<std::uint8_t>(20, 0)));
  EXPECT_EQ(h.egress.ports(), 0b1110U);
  EXPECT_EQ(punts(), 0U);
}

TEST_F(PipelineIgmpTest, ReportsArePuntedAndOnlyForwardedToMrouters) {
  h.tables.vlans[1].mrouters = 0b0010;
  h.receive(2, ip_frame("239.1.1.1", pkt::kIpProtoIgmp, igmp(pkt::kIgmpV2Report, "239.1.1.1")));
  EXPECT_EQ(punts(), 1U);
  EXPECT_EQ(h.egress.ports(), 0b0010U);
}

TEST_F(PipelineIgmpTest, ReportsWithoutMrouterStayLocal) {
  h.receive(2, ip_frame("239.1.1.1", pkt::kIpProtoIgmp, igmp(pkt::kIgmpV2Report, "239.1.1.1")));
  EXPECT_EQ(punts(), 1U);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineIgmpTest, QueriesArePuntedAndFlooded) {
  h.receive(3, ip_frame("224.0.0.1", pkt::kIpProtoIgmp, igmp(pkt::kIgmpQuery, "0.0.0.0")));
  EXPECT_EQ(punts(), 1U);
  EXPECT_EQ(h.egress.ports(), 0b0111U);
}

TEST_F(PipelineIgmpTest, OffloadedUdpChecksumIsCompletedBeforeForwarding) {
  h.tables.vlans[1].flags = shm::vlan_flag::kActive;  // plain flooding is enough here
  std::vector<std::uint8_t> udp(8 + 4, 0);
  pkt::store_be16(udp.data(), 40000);
  pkt::store_be16(udp.data() + 2, 5000);
  pkt::store_be16(udp.data() + 4, 12);
  pkt::store_be16(udp.data() + 6, 0xBEEF);  // partial (pseudo-header) sum left by the sender
  const auto frame = ip_frame("239.1.1.1", pkt::kIpProtoUdp, udp);
  h.receive(0, frame, std::nullopt, /*checksum_incomplete=*/true);
  ASSERT_FALSE(h.egress.sent.empty());
  const auto& out = h.egress.sent[0].frame;
  const auto l4 = std::span{out}.subspan(34, 12);
  EXPECT_EQ(pkt::l4_checksum(*Ipv4Address::parse("10.0.10.11"), *Ipv4Address::parse("239.1.1.1"),
                             pkt::kIpProtoUdp, l4),
            0);
}

TEST_F(PipelineIgmpTest, BroadcastIsUnaffected) {
  const auto f = FrameBuilder{}
                     .mac("ff:ff:ff:ff:ff:ff")
                     .mac("02:00:00:00:00:01")
                     .u16(pkt::kEtherTypeArp)
                     .zeros(46)
                     .build();
  h.receive(0, f);
  EXPECT_EQ(h.egress.ports(), 0b1110U);
}

}  // namespace
}  // namespace softswitch
