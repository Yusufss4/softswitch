#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch {
namespace {

using test::FrameBuilder;
using test::PipelineHarness;

std::vector<std::uint8_t> broadcast_from(const char* src) {
  return FrameBuilder{}.mac("ff:ff:ff:ff:ff:ff").mac(src).u16(pkt::kEtherTypeArp).zeros(46).build();
}

TEST(PipelineFlood, FloodsToAllOtherForwardingPorts) {
  PipelineHarness h(4);
  h.receive(1, broadcast_from("02:00:00:00:00:01"));
  EXPECT_EQ(h.egress.ports(), 0b1101U);
  for (const auto& s : h.egress.sent) {
    EXPECT_FALSE(s.tci.has_value());
  }
  EXPECT_EQ(h.region->dp.ports[1].rx_packets.load(), 1U);
}

TEST(PipelineFlood, SkipsDownAndBlockedPorts) {
  PipelineHarness h(4);
  h.tables.ports[2].flags = shm::port_flag::kAdminUp;  // oper down
  h.tables.ports[3].stp_state = shm::StpState::kDiscarding;
  h.receive(0, broadcast_from("02:00:00:00:00:01"));
  EXPECT_EQ(h.egress.ports(), 0b0010U);
}

TEST(PipelineFlood, DropsOnDownIngressPort) {
  PipelineHarness h(2);
  h.tables.ports[0].flags = 0;
  h.receive(0, broadcast_from("02:00:00:00:00:01"));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(shm::DropReason::kPortDown), 1U);
}

TEST(PipelineFlood, DropsRuntFrames) {
  PipelineHarness h(2);
  h.receive(0, std::vector<std::uint8_t>(10, 0xff));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(shm::DropReason::kMalformed), 1U);
}

TEST(PipelineFlood, InBandTagIsStrippedForUntaggedEgress) {
  PipelineHarness h(2);
  h.tables.ports[0].mode = shm::PortMode::kTrunk;  // tagged ingress needs a trunk
  const auto frame = FrameBuilder{}
                         .mac("ff:ff:ff:ff:ff:ff")
                         .mac("02:00:00:00:00:01")
                         .u16(pkt::kEtherTypeVlan)
                         .u16(pkt::make_tci(1))
                         .u16(pkt::kEtherTypeArp)
                         .zeros(46)
                         .build();
  h.receive(0, frame);
  ASSERT_EQ(h.egress.sent.size(), 1U);
  EXPECT_EQ(h.egress.sent[0].frame.size(), frame.size() - 4);
  EXPECT_EQ(pkt::parse_ethernet(h.egress.sent[0].frame)->tagged, false);
}

// Regression for #47: offloaded UDP checksums must be completed before forwarding.
TEST(PipelineFlood, CompletesOffloadedUdpChecksum) {
  PipelineHarness h(2);
  FrameBuilder b;
  b.mac("ff:ff:ff:ff:ff:ff").mac("02:00:00:00:00:01").u16(pkt::kEtherTypeIpv4);
  b.raw({0x45, 0x00}).u16(20 + 12).raw({0, 1, 0x40, 0x00, 64, pkt::kIpProtoUdp, 0, 0});
  b.ip("10.0.10.11").ip("10.0.10.255");
  b.u16(40000).u16(5000).u16(12).u16(0xBEEF).raw({'p', 'i', 'n', 'g'});
  auto f = b.build();
  pkt::store_be16(f.data() + 24, pkt::internet_checksum(std::span{f}.subspan(14, 20)));
  f.resize(60, 0);
  h.receive(0, f, std::nullopt, /*checksum_incomplete=*/true);
  ASSERT_EQ(h.egress.sent.size(), 1U);
  const auto l4 = std::span{h.egress.sent[0].frame}.subspan(34, 12);
  EXPECT_EQ(pkt::l4_checksum(*Ipv4Address::parse("10.0.10.11"), *Ipv4Address::parse("10.0.10.255"),
                             pkt::kIpProtoUdp, l4),
            0);
}

TEST(PipelineFlood, TracesWhenSamplingEnabled) {
  PipelineHarness h(2);
  h.tables.trace_sample = 1;
  h.receive(0, broadcast_from("02:00:00:00:00:01"));
  shm::TraceRecord rec;
  ASSERT_TRUE(h.region->trace.try_pop(rec));
  EXPECT_EQ(rec.decision, shm::Decision::kFlood);
  EXPECT_EQ(rec.out_ports, 0b10U);
  EXPECT_EQ(rec.src.to_string(), "02:00:00:00:00:01");
}

TEST(PipelineFlood, RawInjectGoesToOnePortUntagged) {
  PipelineHarness h(3);
  shm::PacketMessage msg;
  msg.flags = shm::inject_flag::kRaw;
  msg.port = 2;
  const auto frame = broadcast_from("02:00:00:00:00:fe");
  std::copy(frame.begin(), frame.end(), msg.data.begin());
  msg.len = static_cast<std::uint16_t>(frame.size());
  h.pipeline.process_inject(h.tables, msg, h.egress);
  ASSERT_EQ(h.egress.sent.size(), 1U);
  EXPECT_EQ(h.egress.sent[0].port, 2);
}

}  // namespace
}  // namespace softswitch
