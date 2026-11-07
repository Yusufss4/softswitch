#include <gtest/gtest.h>

#include "softswitch/common/types.hpp"

namespace softswitch {
namespace {

TEST(MacAddress, ParseAndFormatRoundTrip) {
  const auto mac = MacAddress::parse("02:AB:cd:00:10:ff");
  ASSERT_TRUE(mac.has_value());
  EXPECT_EQ(mac->to_string(), "02:ab:cd:00:10:ff");
  EXPECT_EQ(MacAddress::parse("02-ab-cd-00-10-ff"), mac);
}

TEST(MacAddress, RejectsMalformed) {
  EXPECT_FALSE(MacAddress::parse("02:ab:cd:00:10").has_value());
  EXPECT_FALSE(MacAddress::parse("02:ab:cd:00:10:fg").has_value());
  EXPECT_FALSE(MacAddress::parse("02.ab.cd.00.10.ff").has_value());
}

TEST(MacAddress, Classification) {
  EXPECT_TRUE(MacAddress::broadcast().is_broadcast());
  EXPECT_TRUE(MacAddress::broadcast().is_multicast());
  EXPECT_TRUE(MacAddress::parse("01:00:5e:01:02:03")->is_multicast());
  EXPECT_FALSE(MacAddress::parse("02:00:00:00:00:01")->is_multicast());
  EXPECT_TRUE(MacAddress{}.is_zero());
}

TEST(MacAddress, U64RoundTrip) {
  const auto mac = *MacAddress::parse("12:34:56:78:9a:bc");
  EXPECT_EQ(mac.to_u64(), 0x123456789abcULL);
  EXPECT_EQ(MacAddress::from_u64(0x123456789abcULL), mac);
}

TEST(Ipv4Address, ParseAndFormat) {
  const auto ip = Ipv4Address::parse("10.0.20.254");
  ASSERT_TRUE(ip.has_value());
  EXPECT_EQ(ip->value, 0x0A0014FEU);
  EXPECT_EQ(ip->to_string(), "10.0.20.254");
  EXPECT_FALSE(Ipv4Address::parse("10.0.20").has_value());
  EXPECT_FALSE(Ipv4Address::parse("10.0.20.256").has_value());
  EXPECT_FALSE(Ipv4Address::parse("10.0..1").has_value());
  EXPECT_FALSE(Ipv4Address::parse("10.0.0.1.").has_value());
}

TEST(Ipv4Address, MulticastClassification) {
  EXPECT_TRUE(Ipv4Address::parse("239.1.1.1")->is_multicast());
  EXPECT_FALSE(Ipv4Address::parse("239.1.1.1")->is_local_multicast());
  EXPECT_TRUE(Ipv4Address::parse("224.0.0.22")->is_local_multicast());
  EXPECT_FALSE(Ipv4Address::parse("192.168.1.1")->is_multicast());
}

TEST(Ipv4Prefix, ContainsAndParse) {
  const auto p = Ipv4Prefix::parse("10.0.10.1/24");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->network().to_string(), "10.0.10.0");
  EXPECT_TRUE(p->contains(*Ipv4Address::parse("10.0.10.200")));
  EXPECT_FALSE(p->contains(*Ipv4Address::parse("10.0.11.1")));
  EXPECT_EQ(p->to_string(), "10.0.10.1/24");
  EXPECT_TRUE(Ipv4Prefix::parse("0.0.0.0/0")->contains(*Ipv4Address::parse("8.8.8.8")));
  EXPECT_FALSE(Ipv4Prefix::parse("10.0.0.0/33").has_value());
  EXPECT_EQ(prefix_mask(0), 0U);
  EXPECT_EQ(prefix_mask(32), 0xFFFFFFFFU);
  EXPECT_EQ(prefix_mask(20), 0xFFFFF000U);
}

TEST(Multicast, GroupToMac) {
  EXPECT_EQ(multicast_mac(*Ipv4Address::parse("239.129.1.2")).to_string(), "01:00:5e:01:01:02");
}

TEST(PortMask, Bits) {
  EXPECT_EQ(port_bit(0), 1U);
  EXPECT_EQ(port_bit(31), 0x80000000U);
  EXPECT_EQ(port_bit(kMaxPorts), 0U);
  EXPECT_TRUE(has_port(0b1010, 1));
  EXPECT_FALSE(has_port(0b1010, 2));
}

}  // namespace
}  // namespace softswitch
