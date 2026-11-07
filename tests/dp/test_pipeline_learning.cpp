#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/ethernet.hpp"

namespace softswitch {
namespace {

using shm::DropReason;
using test::FrameBuilder;
using test::PipelineHarness;

std::vector<std::uint8_t> frame(const char* dst, const char* src) {
  return FrameBuilder{}.mac(dst).mac(src).u16(pkt::kEtherTypeIpv4).zeros(46).build();
}

constexpr const char* kA = "02:00:00:00:00:0a";
constexpr const char* kB = "02:00:00:00:00:0b";

class PipelineLearningTest : public ::testing::Test {
 protected:
  PipelineHarness h{4};
  void learn_into_tables(const char* mac, PortId port, bool is_static = false) {
    ASSERT_TRUE(shm::fdb_insert(h.tables, *MacAddress::parse(mac), 1, port, is_static));
  }
  std::size_t learn_events() {
    std::size_t n = 0;
    shm::LearnEvent ev;
    while (h.region->learn.try_pop(ev)) {
      ++n;
      last = ev;
    }
    return n;
  }
  shm::LearnEvent last;
};

TEST_F(PipelineLearningTest, UnknownSourceIsReported) {
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 1U);
  EXPECT_EQ(last.mac.to_string(), kA);
  EXPECT_EQ(last.vlan, 1);
  EXPECT_EQ(last.port, 2);
  EXPECT_TRUE(h.pipeline.take_notify());
  EXPECT_FALSE(h.pipeline.take_notify());
}

TEST_F(PipelineLearningTest, RepeatsAreSuppressedUntilTheWindowPasses) {
  h.pipeline.set_now(1'000'000'000);
  h.receive(2, frame(kB, kA));
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 1U);
  h.pipeline.set_now(1'000'000'000 + dp::Pipeline::kLearnRepeatNs + 1);
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 1U);  // still unknown to the CP after the window: report again
}

TEST_F(PipelineLearningTest, MoveIsReportedImmediately) {
  learn_into_tables(kA, 1);
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 1U);
  EXPECT_EQ(last.port, 2);
}

TEST_F(PipelineLearningTest, KnownSourceSetsHitBitInsteadOfReporting) {
  learn_into_tables(kA, 2);
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 0U);
  const auto bit = shm::fdb_hash(*MacAddress::parse(kA), 1) & (shm::kHitBits - 1);
  EXPECT_NE(h.region->dp.fdb_hits[bit / 64].load() & (std::uint64_t{1} << (bit % 64)), 0U);
}

TEST_F(PipelineLearningTest, StaticEntryIsNotOverriddenByTraffic) {
  learn_into_tables(kA, 1, /*is_static=*/true);
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 0U);
}

TEST_F(PipelineLearningTest, GroupSourcesAreNeverLearned) {
  h.receive(2, frame(kB, "01:00:5e:00:00:01"));
  h.receive(2, frame(kB, "00:00:00:00:00:00"));
  EXPECT_EQ(learn_events(), 0U);
}

TEST_F(PipelineLearningTest, KnownUnicastGoesToOnePort) {
  learn_into_tables(kB, 3);
  h.receive(0, frame(kB, kA));
  EXPECT_EQ(h.egress.ports(), 0b1000U);
}

TEST_F(PipelineLearningTest, UnknownUnicastIsFlooded) {
  h.receive(0, frame(kB, kA));
  EXPECT_EQ(h.egress.ports(), 0b1110U);
}

TEST_F(PipelineLearningTest, DestinationOnIngressPortIsFiltered) {
  learn_into_tables(kB, 0);
  h.receive(0, frame(kB, kA));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kSameSegment), 1U);
}

TEST_F(PipelineLearningTest, LearnedPortDownFallsBackToFlooding) {
  learn_into_tables(kB, 3);
  h.tables.ports[3].flags = shm::port_flag::kAdminUp;  // oper down
  h.receive(0, frame(kB, kA));
  EXPECT_EQ(h.egress.ports(), 0b0110U);
}

TEST_F(PipelineLearningTest, BroadcastAlwaysFloods) {
  learn_into_tables("ff:ff:ff:ff:ff:ff", 3);  // nonsense entry must not capture broadcasts
  h.receive(0, frame("ff:ff:ff:ff:ff:ff", kA));
  EXPECT_EQ(h.egress.ports(), 0b1110U);
}

TEST_F(PipelineLearningTest, LearningStatePortLearnsButDoesNotForward) {
  h.tables.ports[2].stp_state = shm::StpState::kLearning;
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 1U);
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kStpBlocked), 1U);
}

TEST_F(PipelineLearningTest, DiscardingPortNeitherLearnsNorForwards) {
  h.tables.ports[2].stp_state = shm::StpState::kDiscarding;
  h.receive(2, frame(kB, kA));
  EXPECT_EQ(learn_events(), 0U);
  EXPECT_TRUE(h.egress.sent.empty());
}

TEST_F(PipelineLearningTest, FullLearnRingCountsDropsButStillForwards) {
  for (std::size_t i = 0; i < decltype(shm::SharedRegion::learn)::kCapacity; ++i) {
    ASSERT_TRUE(h.region->learn.try_push(shm::LearnEvent{}));
  }
  h.receive(0, frame(kB, kA));
  EXPECT_EQ(h.region->learn.drops(), 1U);
  EXPECT_EQ(h.egress.ports(), 0b1110U);  // forwarding never depends on the control plane
}

}  // namespace
}  // namespace softswitch
