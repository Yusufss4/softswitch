#include <gtest/gtest.h>

#include "softswitch/igmp/snooper.hpp"

namespace softswitch::igmp {
namespace {

using namespace std::chrono_literals;

const Ipv4Address kGroup = *Ipv4Address::parse("239.1.1.1");
const Ipv4Address kOther = *Ipv4Address::parse("239.2.2.2");

TEST(Snooper, ReportAddsMemberPerVlanAndGroup) {
  Snooper s;
  const auto t0 = Clock::now();
  EXPECT_TRUE(s.on_report(10, kGroup, 2, t0));
  EXPECT_FALSE(s.on_report(10, kGroup, 2, t0 + 1s));  // refresh, not a change
  EXPECT_TRUE(s.on_report(10, kGroup, 3, t0));
  EXPECT_EQ(s.members(10, kGroup), 0b1100U);
  EXPECT_EQ(s.members(20, kGroup), 0U);
  EXPECT_EQ(s.members(10, kOther), 0U);
}

TEST(Snooper, LinkLocalGroupsAreNeverTracked) {
  Snooper s;
  EXPECT_FALSE(s.on_report(10, *Ipv4Address::parse("224.0.0.251"), 2, Clock::now()));
  EXPECT_FALSE(s.on_report(10, *Ipv4Address::parse("10.0.0.1"), 2, Clock::now()));
  EXPECT_TRUE(s.groups().empty());
}

TEST(Snooper, MembershipExpiresWithoutReports) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);
  EXPECT_FALSE(s.tick(t0 + 259s));
  EXPECT_TRUE(s.tick(t0 + 261s));
  EXPECT_EQ(s.members(10, kGroup), 0U);
  EXPECT_TRUE(s.groups().empty());
}

TEST(Snooper, FastLeaveRemovesImmediately) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);
  s.on_report(10, kGroup, 3, t0);
  EXPECT_TRUE(s.on_leave(10, kGroup, 2, /*fast_leave=*/true, t0));
  EXPECT_EQ(s.members(10, kGroup), 0b1000U);
}

TEST(Snooper, NormalLeaveWaitsForOtherMembersToReReport) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);  // a trunk with possibly several listeners behind it
  EXPECT_FALSE(s.on_leave(10, kGroup, 2, /*fast_leave=*/false, t0));
  EXPECT_EQ(s.members(10, kGroup), 0b0100U);  // still forwarding during the grace period
  // Another listener behind the same port answers the group-specific query in time:
  s.on_report(10, kGroup, 2, t0 + 1s);
  EXPECT_FALSE(s.tick(t0 + 3s));
  EXPECT_EQ(s.members(10, kGroup), 0b0100U);
}

TEST(Snooper, NormalLeaveWithoutReReportExpiresQuickly) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);
  s.on_leave(10, kGroup, 2, false, t0);
  EXPECT_TRUE(s.tick(t0 + 3s));
  EXPECT_EQ(s.members(10, kGroup), 0U);
}

TEST(Snooper, QueriesMarkMrouterPortsThatExpire) {
  Snooper s;
  const auto t0 = Clock::now();
  EXPECT_TRUE(s.on_query(10, 4, t0));
  EXPECT_FALSE(s.on_query(10, 4, t0 + 100s));
  EXPECT_EQ(s.mrouters(10), 0b10000U);
  EXPECT_EQ(s.mrouters(20), 0U);
  EXPECT_TRUE(s.tick(t0 + 401s));
  EXPECT_EQ(s.mrouters(10), 0U);
}

TEST(Snooper, PortDownForgetsEverythingOnIt) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);
  s.on_report(10, kGroup, 3, t0);
  s.on_query(10, 2, t0);
  EXPECT_TRUE(s.on_port_down(2));
  EXPECT_EQ(s.members(10, kGroup), 0b1000U);
  EXPECT_EQ(s.mrouters(10), 0U);
  EXPECT_FALSE(s.on_port_down(2));
}

TEST(Snooper, ClearVlan) {
  Snooper s;
  const auto t0 = Clock::now();
  s.on_report(10, kGroup, 2, t0);
  s.on_report(20, kGroup, 2, t0);
  EXPECT_TRUE(s.clear_vlan(10));
  EXPECT_EQ(s.members(10, kGroup), 0U);
  EXPECT_EQ(s.members(20, kGroup), 0b0100U);
}

}  // namespace
}  // namespace softswitch::igmp
