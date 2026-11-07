#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/ethernet.hpp"

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
