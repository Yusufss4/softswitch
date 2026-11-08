#include <gtest/gtest.h>

#include "frame_builder.hpp"
#include "pipeline_harness.hpp"
#include "softswitch/pkt/ethernet.hpp"

namespace softswitch {
namespace {

using shm::DropReason;
using shm::PortMode;
using test::FrameBuilder;
using test::PipelineHarness;

// Port layout used by every test:
//   p0: access VLAN 10    p1: access VLAN 20    p2: trunk {10,20}, native 10    p3: trunk {20}, no
//   native
class PipelineVlanTest : public ::testing::Test {
 protected:
  PipelineHarness h{4};

  void SetUp() override {
    auto& t = h.tables;
    t.vlans.fill(shm::VlanEntry{});
    t.vlans[10].flags = shm::vlan_flag::kActive;
    t.vlans[20].flags = shm::vlan_flag::kActive;
    t.ports[0].pvid = 10;
    t.ports[1].pvid = 20;
    t.ports[2].mode = PortMode::kTrunk;
    t.ports[2].pvid = 10;
    t.ports[3].mode = PortMode::kTrunk;
    t.ports[3].pvid = 0;
    t.vlans[10].members = 0b0101;
    t.vlans[10].untagged = 0b0101;  // p0 access, p2 native
    t.vlans[20].members = 0b1110;
    t.vlans[20].untagged = 0b0010;  // p1 access
  }

  static std::vector<std::uint8_t> untagged(const char* dst = "ff:ff:ff:ff:ff:ff") {
    return FrameBuilder{}
        .mac(dst)
        .mac("02:00:00:00:00:01")
        .u16(pkt::kEtherTypeArp)
        .zeros(46)
        .build();
  }
  static std::vector<std::uint8_t> tagged(VlanId vid) {
    return FrameBuilder{}
        .mac("ff:ff:ff:ff:ff:ff")
        .mac("02:00:00:00:00:01")
        .u16(pkt::kEtherTypeVlan)
        .u16(pkt::make_tci(vid))
        .u16(pkt::kEtherTypeArp)
        .zeros(46)
        .build();
  }
  const test::RecordingEgress::Sent* sent_to(PortId p) const {
    for (const auto& s : h.egress.sent) {
      if (s.port == p) {
        return &s;
      }
    }
    return nullptr;
  }
};

TEST_F(PipelineVlanTest, UntaggedOnAccessGoesToVlanMembersOnly) {
  h.receive(0, untagged());
  EXPECT_EQ(h.egress.ports(), 0b0100U);  // only the trunk; never p1 (VLAN 20)
  ASSERT_NE(sent_to(2), nullptr);
  EXPECT_FALSE(sent_to(2)->tci.has_value());  // VLAN 10 is the trunk's native VLAN
}

TEST_F(PipelineVlanTest, TrunkTagsNonNativeVlans) {
  h.receive(1, untagged());  // VLAN 20 from p1
  EXPECT_EQ(h.egress.ports(), 0b1100U);
  ASSERT_NE(sent_to(2), nullptr);
  ASSERT_TRUE(sent_to(2)->tci.has_value());
  EXPECT_EQ(pkt::tci_vid(*sent_to(2)->tci), 20);
  ASSERT_NE(sent_to(3), nullptr);
  EXPECT_EQ(pkt::tci_vid(*sent_to(3)->tci), 20);
}

TEST_F(PipelineVlanTest, TaggedOnTrunkIsStrippedTowardsAccess) {
  h.receive(2, tagged(20), std::nullopt);  // in-band tag
  EXPECT_EQ(h.egress.ports(), 0b1010U);
  ASSERT_NE(sent_to(1), nullptr);
  EXPECT_FALSE(sent_to(1)->tci.has_value());
  EXPECT_FALSE(pkt::parse_ethernet(sent_to(1)->frame)->tagged);
}

TEST_F(PipelineVlanTest, OffloadedTagIsHonoured) {
  h.receive(2, untagged(), pkt::make_tci(20));  // AF_PACKET moved the tag to metadata
  EXPECT_EQ(h.egress.ports(), 0b1010U);
}

TEST_F(PipelineVlanTest, UntaggedOnTrunkUsesNativeVlan) {
  h.receive(2, untagged());
  EXPECT_EQ(h.egress.ports(), 0b0001U);  // VLAN 10 -> p0
}

TEST_F(PipelineVlanTest, TaggedFrameOnAccessPortIsDropped) {
  h.receive(0, tagged(10));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kTaggedOnAccess), 1U);
}

TEST_F(PipelineVlanTest, PriorityTaggedFrameOnAccessIsAccepted) {
  h.receive(0, untagged(), pkt::make_tci(0, 5));  // VID 0, PCP 5
  EXPECT_EQ(h.egress.ports(), 0b0100U);
}

// Stacked 802.1Q tags (found by fuzz_pipeline): after the outer tag is classified and removed,
// the frame must not still begin with a tag. Forwarding it would let the inner tag pick the
// VLAN at the next switch: the double-tagging VLAN-hopping attack.
std::vector<std::uint8_t> double_tagged(VlanId outer, VlanId inner) {
  return FrameBuilder{}
      .mac("ff:ff:ff:ff:ff:ff")
      .mac("02:00:00:00:00:66")
      .u16(pkt::kEtherTypeVlan)
      .u16(pkt::make_tci(outer))
      .u16(pkt::kEtherTypeVlan)
      .u16(pkt::make_tci(inner))
      .u16(pkt::kEtherTypeArp)
      .zeros(46)
      .build();
}

TEST_F(PipelineVlanTest, PriorityTagHidingAnInnerTagIsDropped) {
  // Attacker on access VLAN 10 sends priority tag + VID 20, hoping p2 (native 10, untagged)
  // forwards the inner VID 20 tag to the next switch.
  h.receive(0, double_tagged(0, 20));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kStackedTag), 1U);
}

TEST_F(PipelineVlanTest, OffloadedTagWithAnInnerTagIsDropped) {
  // AF_PACKET moved the outer tag to metadata; the frame itself still starts with a tag.
  h.receive(2, tagged(10), pkt::make_tci(20));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kStackedTag), 1U);
}

TEST_F(PipelineVlanTest, StackedTagsOnATrunkAreDropped) {
  h.receive(2, double_tagged(20, 10));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kStackedTag), 1U);
}

TEST_F(PipelineVlanTest, VlanNotAllowedOnTrunkIsDropped) {
  h.receive(3, tagged(10));  // p3 only carries VLAN 20
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kVlanNotMember), 1U);
}

TEST_F(PipelineVlanTest, UnknownVlanIsDropped) {
  h.receive(2, tagged(99));
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kVlanNotMember), 1U);
}

TEST_F(PipelineVlanTest, UntaggedOnTrunkWithoutNativeIsDropped) {
  h.receive(3, untagged());
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kUntaggedNotAllowed), 1U);
}

TEST_F(PipelineVlanTest, BpdusArePuntedRegardlessOfVlanAndStpState) {
  h.tables.flags = shm::global_flag::kStpEnabled;
  h.tables.ports[3].stp_state = shm::StpState::kDiscarding;
  h.receive(3, untagged("01:80:c2:00:00:00"));  // p3: trunk without native VLAN, discarding
  EXPECT_TRUE(h.egress.sent.empty());
  shm::PacketMessage msg;
  ASSERT_TRUE(h.region->punt.try_pop(msg));
  EXPECT_EQ(msg.port, 3);
  EXPECT_EQ(msg.reason, shm::PuntReason::kBpdu);
  EXPECT_EQ(msg.len, 60);
  EXPECT_TRUE(h.pipeline.take_notify());
}

TEST_F(PipelineVlanTest, FullPuntRingDropsBpdusWithoutBlocking) {
  h.tables.flags = shm::global_flag::kStpEnabled;
  while (h.region->punt.reserve() != nullptr) {
    h.region->punt.commit();
  }
  h.receive(0, untagged("01:80:c2:00:00:00"));
  EXPECT_EQ(h.drops(DropReason::kPuntRingFull), 1U);
}

TEST_F(PipelineVlanTest, ReservedMulticastIsNeverForwarded) {
  for (const char* dst : {"01:80:c2:00:00:00", "01:80:c2:00:00:02", "01:80:c2:00:00:0e"}) {
    h.receive(0, untagged(dst));
  }
  EXPECT_TRUE(h.egress.sent.empty());
  EXPECT_EQ(h.drops(DropReason::kReservedMulticast), 3U);
  h.receive(0, untagged("01:80:c2:00:00:10"));  // outside the reserved block: flooded
  EXPECT_EQ(h.egress.ports(), 0b0100U);
}

TEST_F(PipelineVlanTest, TraceRecordsTheDropReason) {
  h.tables.trace_sample = 1;
  h.receive(0, tagged(10));
  shm::TraceRecord rec;
  ASSERT_TRUE(h.region->trace.try_pop(rec));
  EXPECT_EQ(rec.decision, shm::Decision::kDrop);
  EXPECT_EQ(rec.reason, DropReason::kTaggedOnAccess);
}

}  // namespace
}  // namespace softswitch
