#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "frame_builder.hpp"
#include "softswitch/pkt/arp.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/icmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::pkt {
namespace {

using test::FrameBuilder;

// Classic example header (Wikipedia / RFC 1071 style): checksum is 0xb861.
std::vector<std::uint8_t> sample_ip_header() {
  return FrameBuilder{}
      .raw({0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb8, 0x61})
      .ip("192.168.0.1")
      .ip("192.168.0.199")
      .zeros(0x73 - 20)
      .build();
}

TEST(Ipv4, ChecksumKnownVector) {
  auto h = sample_ip_header();
  EXPECT_TRUE(ipv4_header_ok(h));
  store_be16(h.data() + 10, 0);
  EXPECT_EQ(internet_checksum(std::span{h}.first(20)), 0xb861);
}

TEST(Ipv4, ParseFields) {
  const auto h = sample_ip_header();
  const auto ip = parse_ipv4(h);
  ASSERT_TRUE(ip.has_value());
  EXPECT_EQ(ip->header_len, 20);
  EXPECT_EQ(ip->total_len, 0x73);
  EXPECT_EQ(ip->ttl, 64);
  EXPECT_EQ(ip->protocol, kIpProtoUdp);
  EXPECT_EQ(ip->src.to_string(), "192.168.0.1");
  EXPECT_EQ(ip->dst.to_string(), "192.168.0.199");
}

TEST(Ipv4, RejectsBadHeaders) {
  auto h = sample_ip_header();
  h[0] = 0x65;  // version 6
  EXPECT_FALSE(parse_ipv4(h).has_value());
  h = sample_ip_header();
  h[0] = 0x44;  // IHL 4 (< 20 bytes)
  EXPECT_FALSE(parse_ipv4(h).has_value());
  h = sample_ip_header();
  h.resize(0x72);  // shorter than total length
  EXPECT_FALSE(parse_ipv4(h).has_value());
  h = sample_ip_header();
  h[11] ^= 0x01;  // corrupt checksum
  EXPECT_TRUE(parse_ipv4(h).has_value());
  EXPECT_FALSE(ipv4_header_ok(h));
}

TEST(Ipv4, IncrementalTtlMatchesFullRecompute) {
  for (const int ttl_value : {2, 64, 128, 255}) {
    const auto ttl = static_cast<std::uint8_t>(ttl_value);
    auto h = sample_ip_header();
    h[8] = ttl;
    store_be16(h.data() + 10, 0);
    store_be16(h.data() + 10, internet_checksum(std::span{h}.first(20)));
    EXPECT_EQ(decrement_ttl(h), ttl - 1);
    EXPECT_TRUE(ipv4_header_ok(h)) << "ttl " << int{ttl};
  }
}

TEST(Ipv4, ChecksumAdjustAgreesWithRfc1624Example) {
  // RFC 1624 §4: HC = 0xDD2F, m = 0x5555 -> m' = 0x3285 gives HC' = 0x0000.
  EXPECT_EQ(checksum_adjust(0xDD2F, 0x5555, 0x3285), 0x0000);
}

TEST(Ipv4, OddLengthChecksum) {
  const std::array<std::uint8_t, 3> data{0x01, 0x02, 0x03};
  // 0x0102 + 0x0300 = 0x0402 -> ~ = 0xFBFD
  EXPECT_EQ(internet_checksum(data), 0xFBFD);
}

TEST(Ipv4, WriteHeaderProducesValidChecksum) {
  std::array<std::uint8_t, 20> out{};
  Ipv4Header h;
  h.total_len = 20;
  h.ttl = 64;
  h.protocol = kIpProtoIcmp;
  h.src = *Ipv4Address::parse("10.0.0.1");
  h.dst = *Ipv4Address::parse("10.0.0.2");
  ASSERT_EQ(write_ipv4_header(out, h), 20U);
  EXPECT_TRUE(ipv4_header_ok(out));
}

std::vector<std::uint8_t> echo_request() {
  FrameBuilder b;
  b.mac("02:00:00:00:00:fe").mac("02:00:00:00:00:01").u16(kEtherTypeIpv4);
  b.raw({0x45, 0x00}).u16(20 + 8 + 4).raw({0x12, 0x34, 0x40, 0x00, 0x40, kIpProtoIcmp, 0, 0});
  b.ip("10.0.10.2").ip("10.0.10.1");
  b.raw({kIcmpEchoRequest, 0, 0, 0}).u16(0x0042).u16(7).raw({'p', 'i', 'n', 'g'});
  auto f = b.build();
  store_be16(f.data() + 14 + 10, internet_checksum(std::span{f}.subspan(14, 20)));
  store_be16(f.data() + 34 + 2, internet_checksum(std::span{f}.subspan(34, 12)));
  return f;
}

TEST(Icmp, EchoRequestBecomesReply) {
  auto f = echo_request();
  ASSERT_TRUE(echo_request_to_reply(f));
  const auto eth = parse_ethernet(f);
  EXPECT_EQ(eth->dst.to_string(), "02:00:00:00:00:01");
  EXPECT_EQ(eth->src.to_string(), "02:00:00:00:00:fe");
  const auto l3 = std::span{f}.subspan(14);
  const auto ip = parse_ipv4(l3);
  EXPECT_EQ(ip->src.to_string(), "10.0.10.1");
  EXPECT_EQ(ip->dst.to_string(), "10.0.10.2");
  EXPECT_TRUE(ipv4_header_ok(l3));
  const auto l4 = l3.subspan(20, 12);
  const auto icmp = parse_icmp(l4);
  EXPECT_EQ(icmp->type, kIcmpEchoReply);
  EXPECT_EQ(icmp->id, 0x42);
  EXPECT_EQ(icmp->seq, 7);
  EXPECT_EQ(internet_checksum(l4), 0);
  EXPECT_EQ(l4[8], 'p');
}

TEST(Icmp, EchoReplyIsNotTurnedAround) {
  auto f = echo_request();
  ASSERT_TRUE(echo_request_to_reply(f));
  const auto copy = f;
  EXPECT_FALSE(echo_request_to_reply(f));
  EXPECT_EQ(f, copy);
}

TEST(Icmp, TimeExceededQuotesOffendingHeader) {
  const auto req = echo_request();
  const auto offending = std::span{req}.subspan(14);
  std::array<std::uint8_t, 128> out{};
  const auto mac_a = *MacAddress::parse("02:00:00:00:00:01");
  const auto mac_r = *MacAddress::parse("02:00:00:00:00:fe");
  const auto len = build_icmp_error_frame(out, mac_a, mac_r, *Ipv4Address::parse("10.0.10.1"),
                                          kIcmpTimeExceeded, 0, offending);
  ASSERT_EQ(len, 14U + 20U + 8U + 28U);
  const auto l3 = std::span{out}.subspan(14, len - 14);
  const auto ip = parse_ipv4(l3);
  ASSERT_TRUE(ip.has_value());
  EXPECT_TRUE(ipv4_header_ok(l3));
  EXPECT_EQ(ip->dst.to_string(), "10.0.10.2");
  const auto l4 = l3.subspan(20, ip->total_len - 20U);
  EXPECT_EQ(l4[0], kIcmpTimeExceeded);
  EXPECT_EQ(internet_checksum(l4), 0);
  EXPECT_EQ(l4[8], 0x45);  // quoted IP header starts here
}

TEST(Arp, BuildThenParse) {
  ArpPacket arp;
  arp.op = kArpOpRequest;
  arp.sender_mac = *MacAddress::parse("02:00:00:00:00:fe");
  arp.sender_ip = *Ipv4Address::parse("10.0.10.1");
  arp.target_ip = *Ipv4Address::parse("10.0.10.2");
  std::array<std::uint8_t, 64> out{};
  const auto len = build_arp_frame(out, MacAddress::broadcast(), arp.sender_mac, arp);
  ASSERT_EQ(len, kMinFrameLen);
  const auto eth = parse_ethernet(std::span{out}.first(len));
  ASSERT_TRUE(eth.has_value());
  EXPECT_EQ(eth->ethertype, kEtherTypeArp);
  const auto parsed = parse_arp(std::span{out}.subspan(eth->payload_offset));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->op, kArpOpRequest);
  EXPECT_EQ(parsed->sender_mac, arp.sender_mac);
  EXPECT_EQ(parsed->sender_ip, arp.sender_ip);
  EXPECT_EQ(parsed->target_ip, arp.target_ip);
  EXPECT_TRUE(parsed->target_mac.is_zero());
}

TEST(Arp, RejectsNonEthernetIpv4) {
  auto payload = FrameBuilder{}.u16(6).u16(kEtherTypeIpv4).raw({6, 4}).zeros(22).build();
  EXPECT_FALSE(parse_arp(payload).has_value());
  payload = FrameBuilder{}.u16(1).u16(kEtherTypeIpv4).raw({6, 4}).zeros(10).build();
  EXPECT_FALSE(parse_arp(payload).has_value());
}

}  // namespace
}  // namespace softswitch::pkt
