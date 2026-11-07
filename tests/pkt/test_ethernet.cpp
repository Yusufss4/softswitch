#include <gtest/gtest.h>

#include <vector>

#include "frame_builder.hpp"
#include "softswitch/pkt/ethernet.hpp"

namespace softswitch::pkt {
namespace {

using test::FrameBuilder;

std::vector<std::uint8_t> untagged_frame() {
  return FrameBuilder{}
      .mac("ff:ff:ff:ff:ff:ff")
      .mac("02:00:00:00:00:01")
      .u16(kEtherTypeArp)
      .raw({0xde, 0xad, 0xbe, 0xef})
      .build();
}

TEST(Ethernet, ParsesUntagged) {
  const auto f = untagged_frame();
  const auto v = parse_ethernet(f);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->dst.is_broadcast());
  EXPECT_EQ(v->src.to_string(), "02:00:00:00:00:01");
  EXPECT_FALSE(v->tagged);
  EXPECT_EQ(v->ethertype, kEtherTypeArp);
  EXPECT_EQ(v->payload_offset, kEthHeaderLen);
}

TEST(Ethernet, ParsesTagged) {
  const auto f = FrameBuilder{}
                     .mac("02:00:00:00:00:02")
                     .mac("02:00:00:00:00:01")
                     .u16(kEtherTypeVlan)
                     .u16(make_tci(20, 5))
                     .u16(kEtherTypeIpv4)
                     .zeros(20)
                     .build();
  const auto v = parse_ethernet(f);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->tagged);
  EXPECT_EQ(tci_vid(v->tci), 20);
  EXPECT_EQ(tci_pcp(v->tci), 5);
  EXPECT_EQ(v->ethertype, kEtherTypeIpv4);
  EXPECT_EQ(v->payload_offset, 18U);
}

TEST(Ethernet, RejectsTruncated) {
  std::vector<std::uint8_t> f(13, 0);
  EXPECT_FALSE(parse_ethernet(f).has_value());
  auto tagged =
      FrameBuilder{}.mac("02:00:00:00:00:02").mac("02:00:00:00:00:01").u16(0x8100).u8(0).build();
  EXPECT_FALSE(parse_ethernet(tagged).has_value());
}

TEST(Ethernet, InsertTagIsByteExact) {
  auto f = untagged_frame();
  const auto original = f;
  f.resize(f.size() + kVlanTagLen);
  const auto len = insert_tag(f, original.size(), make_tci(10));
  ASSERT_EQ(len, original.size() + kVlanTagLen);
  const auto expected = FrameBuilder{}
                            .mac("ff:ff:ff:ff:ff:ff")
                            .mac("02:00:00:00:00:01")
                            .u16(kEtherTypeVlan)
                            .u16(10)
                            .u16(kEtherTypeArp)
                            .raw({0xde, 0xad, 0xbe, 0xef})
                            .build();
  EXPECT_EQ(f, expected);
}

TEST(Ethernet, StripTagRestoresOriginal) {
  auto f = untagged_frame();
  const auto original = f;
  f.resize(f.size() + kVlanTagLen);
  ASSERT_NE(insert_tag(f, original.size(), make_tci(100, 3)), 0U);
  const auto len = strip_tag(f);
  ASSERT_EQ(len, original.size());
  f.resize(len);
  EXPECT_EQ(f, original);
}

TEST(Ethernet, StripUntaggedFails) {
  auto f = untagged_frame();
  EXPECT_EQ(strip_tag(f), 0U);
}

TEST(Ethernet, InsertTagNeedsRoom) {
  auto f = untagged_frame();
  EXPECT_EQ(insert_tag(f, f.size(), make_tci(10)), 0U);
}

TEST(Ethernet, CopyWithTagMatchesInsertTag) {
  const auto f = untagged_frame();
  std::vector<std::uint8_t> copied(f.size() + kVlanTagLen);
  ASSERT_EQ(copy_with_tag(f, make_tci(42), copied), f.size() + kVlanTagLen);
  auto inserted = f;
  inserted.resize(f.size() + kVlanTagLen);
  ASSERT_NE(insert_tag(inserted, f.size(), make_tci(42)), 0U);
  EXPECT_EQ(copied, inserted);
}

TEST(Ethernet, PadFrame) {
  std::vector<std::uint8_t> buf(64, 0xAA);
  EXPECT_EQ(pad_frame(buf, 42), kMinFrameLen);
  EXPECT_EQ(buf[59], 0);
  EXPECT_EQ(buf[60], 0xAA);
  EXPECT_EQ(pad_frame(buf, 61), 61U);
}

}  // namespace
}  // namespace softswitch::pkt
