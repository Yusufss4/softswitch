#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "frame_builder.hpp"
#include "softswitch/pkt/bpdu.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/igmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::pkt {
namespace {

using test::FrameBuilder;

std::vector<std::uint8_t> with_checksum(std::vector<std::uint8_t> igmp) {
  store_be16(igmp.data() + 2, 0);
  store_be16(igmp.data() + 2, internet_checksum(igmp));
  return igmp;
}

TEST(Igmp, ParsesV2ReportAndLeave) {
  const auto report =
      with_checksum(FrameBuilder{}.raw({kIgmpV2Report, 0, 0, 0}).ip("239.1.1.1").build());
  const auto msg = parse_igmp(report);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->type, kIgmpV2Report);
  EXPECT_EQ(msg->group.to_string(), "239.1.1.1");

  const auto leave =
      with_checksum(FrameBuilder{}.raw({kIgmpV2Leave, 0, 0, 0}).ip("239.1.1.1").build());
  EXPECT_EQ(parse_igmp(leave)->type, kIgmpV2Leave);
}

TEST(Igmp, RejectsBadChecksum) {
  auto report = with_checksum(FrameBuilder{}.raw({kIgmpV2Report, 0, 0, 0}).ip("239.1.1.1").build());
  report[7] ^= 0xff;
  EXPECT_FALSE(parse_igmp(report).has_value());
}

TEST(Igmp, ParsesV3ReportRecords) {
  FrameBuilder b;
  b.raw({kIgmpV3Report, 0, 0, 0, 0, 0}).u16(3);
  b.raw({kIgmpChangeToExclude, 0}).u16(0).ip("239.1.1.1");                      // join
  b.raw({kIgmpChangeToInclude, 0}).u16(0).ip("239.2.2.2");                      // leave
  b.raw({kIgmpModeIsInclude, 1}).u16(1).ip("232.1.1.1").ip("10.0.0.9").u32(0);  // SSM join + aux
  const auto msg = parse_igmp(with_checksum(b.build()));
  ASSERT_TRUE(msg.has_value());
  ASSERT_EQ(msg->num_records, 3U);
  EXPECT_TRUE(IgmpMessage::record_is_join(msg->records[0]));
  EXPECT_FALSE(IgmpMessage::record_is_join(msg->records[1]));
  EXPECT_TRUE(IgmpMessage::record_is_join(msg->records[2]));
  EXPECT_EQ(msg->records[2].group.to_string(), "232.1.1.1");
}

TEST(Igmp, RejectsTruncatedV3Record) {
  FrameBuilder b;
  b.raw({kIgmpV3Report, 0, 0, 0, 0, 0}).u16(1);
  b.raw({kIgmpModeIsExclude, 0}).u16(2).ip("239.1.1.1").ip("10.0.0.1");  // claims 2 sources
  EXPECT_FALSE(parse_igmp(with_checksum(b.build())).has_value());
}

TEST(Igmp, QueryFrameIsWellFormed) {
  std::array<std::uint8_t, 64> out{};
  const auto src = *MacAddress::parse("02:00:00:00:00:fe");
  const auto len = build_igmp_query_frame(out, src, *Ipv4Address::parse("10.0.10.1"), {}, 100);
  ASSERT_EQ(len, kMinFrameLen);
  const auto eth = parse_ethernet(std::span{out}.first(len));
  EXPECT_EQ(eth->dst.to_string(), "01:00:5e:00:00:01");
  const auto l3 = std::span{out}.subspan(kEthHeaderLen);
  const auto ip = parse_ipv4(l3);
  ASSERT_TRUE(ip.has_value());
  EXPECT_TRUE(ipv4_header_ok(l3));
  EXPECT_EQ(ip->ttl, 1);
  EXPECT_EQ(ip->header_len, 24);
  EXPECT_EQ(ip->dst, kAllHostsGroup);
  const auto msg = parse_igmp(l3.subspan(ip->header_len, ip->total_len - ip->header_len));
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->type, kIgmpQuery);
  EXPECT_EQ(msg->max_resp, 100);
  EXPECT_TRUE(msg->group.is_zero());
}

Bpdu sample_rst() {
  Bpdu b;
  b.flags = bpdu_flag::kProposal | bpdu_flag::kLearning | bpdu_flag::kForwarding;
  b.set_role(BpduRole::kDesignated);
  b.root_id = 0x1000'0200'0000'0001ULL;
  b.root_path_cost = 20000;
  b.bridge_id = 0x8000'0200'0000'0002ULL;
  b.port_id = 0x8003;
  b.message_age = seconds_to_bpdu_time(1);
  b.max_age = seconds_to_bpdu_time(20);
  b.hello_time = seconds_to_bpdu_time(2);
  b.forward_delay = seconds_to_bpdu_time(15);
  return b;
}

TEST(Bpdu, RstRoundTrip) {
  std::array<std::uint8_t, 64> out{};
  const auto src = *MacAddress::parse("02:00:00:00:00:02");
  const auto len = build_bpdu_frame(out, src, sample_rst());
  ASSERT_EQ(len, kMinFrameLen);
  const auto eth = parse_ethernet(std::span{out}.first(len));
  EXPECT_EQ(eth->dst, kStpGroupMac);
  EXPECT_EQ(eth->ethertype, 39);  // 802.3 length: LLC (3) + RST BPDU (36)
  const auto parsed = parse_bpdu_frame(std::span{out}.first(len));
  ASSERT_TRUE(parsed.has_value());
  const auto expected = sample_rst();
  EXPECT_EQ(parsed->type, kBpduTypeRst);
  EXPECT_EQ(parsed->version, kBpduVersionRstp);
  EXPECT_EQ(parsed->flags, expected.flags);
  EXPECT_EQ(parsed->role(), BpduRole::kDesignated);
  EXPECT_TRUE(parsed->has(bpdu_flag::kProposal));
  EXPECT_FALSE(parsed->has(bpdu_flag::kAgreement));
  EXPECT_EQ(parsed->root_id, expected.root_id);
  EXPECT_EQ(parsed->root_path_cost, expected.root_path_cost);
  EXPECT_EQ(parsed->bridge_id, expected.bridge_id);
  EXPECT_EQ(parsed->port_id, expected.port_id);
  EXPECT_EQ(parsed->message_age, expected.message_age);
  EXPECT_EQ(parsed->max_age, expected.max_age);
  EXPECT_EQ(parsed->hello_time, expected.hello_time);
  EXPECT_EQ(parsed->forward_delay, expected.forward_delay);
}

TEST(Bpdu, TcnRoundTrip) {
  std::array<std::uint8_t, 64> out{};
  Bpdu tcn;
  tcn.version = kBpduVersionStp;
  tcn.type = kBpduTypeTcn;
  const auto len = build_bpdu_frame(out, *MacAddress::parse("02:00:00:00:00:02"), tcn);
  const auto parsed = parse_bpdu_frame(std::span{out}.first(len));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->type, kBpduTypeTcn);
}

TEST(Bpdu, RejectsWrongLlcOrDestination) {
  std::array<std::uint8_t, 64> out{};
  const auto len = build_bpdu_frame(out, *MacAddress::parse("02:00:00:00:00:02"), sample_rst());
  auto bad = out;
  bad[kEthHeaderLen] = 0xAA;  // SNAP, not STP
  EXPECT_FALSE(parse_bpdu_frame(std::span{bad}.first(len)).has_value());
  bad = out;
  bad[5] = 0x0E;  // 01:80:c2:00:00:0e (LLDP), not the bridge group address
  EXPECT_FALSE(parse_bpdu_frame(std::span{bad}.first(len)).has_value());
  bad = out;
  store_be16(bad.data() + kMacAddrsLen, 3 + 20);  // length too short for an RST BPDU
  EXPECT_FALSE(parse_bpdu_frame(std::span{bad}.first(len)).has_value());
}

TEST(Bpdu, RoleEncoding) {
  Bpdu b;
  for (auto role :
       {BpduRole::kUnknown, BpduRole::kAlternateBackup, BpduRole::kRoot, BpduRole::kDesignated}) {
    b.flags = bpdu_flag::kAgreement | bpdu_flag::kTopologyChange;
    b.set_role(role);
    EXPECT_EQ(b.role(), role);
    EXPECT_TRUE(b.has(bpdu_flag::kAgreement));
    EXPECT_TRUE(b.has(bpdu_flag::kTopologyChange));
  }
}

}  // namespace
}  // namespace softswitch::pkt
