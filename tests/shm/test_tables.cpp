#include <gtest/gtest.h>

#include <memory>

#include "softswitch/shm/tables.hpp"

namespace softswitch::shm {
namespace {

MacAddress mac(std::uint64_t n) { return MacAddress::from_u64(0x020000000000ULL | n); }
Ipv4Address ip(const char* s) { return *Ipv4Address::parse(s); }

class TablesTest : public ::testing::Test {
 protected:
  std::unique_ptr<Tables> t_ = std::make_unique<Tables>();
};

TEST_F(TablesTest, FdbInsertLookupPerVlan) {
  ASSERT_TRUE(fdb_insert(*t_, mac(1), 10, 3, false));
  ASSERT_TRUE(fdb_insert(*t_, mac(1), 20, 4, true));
  const auto* a = fdb_lookup(*t_, mac(1), 10);
  const auto* b = fdb_lookup(*t_, mac(1), 20);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->port, 3);
  EXPECT_EQ(b->port, 4);
  EXPECT_NE(b->flags & slot_flag::kStatic, 0);
  EXPECT_EQ(fdb_lookup(*t_, mac(1), 30), nullptr);
  EXPECT_EQ(t_->fdb_count, 2U);
}

TEST_F(TablesTest, FdbOverwriteDoesNotGrow) {
  ASSERT_TRUE(fdb_insert(*t_, mac(1), 10, 3, false));
  ASSERT_TRUE(fdb_insert(*t_, mac(1), 10, 5, false));
  EXPECT_EQ(fdb_lookup(*t_, mac(1), 10)->port, 5);
  EXPECT_EQ(t_->fdb_count, 1U);
}

TEST_F(TablesTest, FdbStopsAtLoadFactorLimit) {
  std::size_t inserted = 0;
  for (std::uint64_t i = 0; i < kFdbSlots; ++i) {
    if (!fdb_insert(*t_, mac(i), 1, 0, false)) {
      break;
    }
    ++inserted;
  }
  EXPECT_EQ(inserted, kFdbSlots / 2);
  // Every inserted entry is still reachable despite collisions.
  for (std::uint64_t i = 0; i < inserted; ++i) {
    ASSERT_NE(fdb_lookup(*t_, mac(i), 1), nullptr) << i;
  }
  EXPECT_EQ(fdb_lookup(*t_, mac(kFdbSlots), 1), nullptr);
}

TEST_F(TablesTest, ClearDynamicEmptiesHashTables) {
  ASSERT_TRUE(fdb_insert(*t_, mac(1), 10, 3, false));
  ASSERT_TRUE(mdb_insert(*t_, ip("239.1.1.1"), 10, 0b110));
  ASSERT_TRUE(neighbor_insert(*t_, ip("10.0.10.2"), mac(2), 10));
  t_->ports[0].pvid = 77;
  clear_dynamic(*t_);
  EXPECT_EQ(fdb_lookup(*t_, mac(1), 10), nullptr);
  EXPECT_EQ(mdb_lookup(*t_, ip("239.1.1.1"), 10), nullptr);
  EXPECT_EQ(neighbor_lookup(*t_, ip("10.0.10.2")), nullptr);
  EXPECT_EQ(t_->fdb_count, 0U);
  EXPECT_EQ(t_->ports[0].pvid, 77);  // static config untouched
}

TEST_F(TablesTest, MdbAndNeighbors) {
  ASSERT_TRUE(mdb_insert(*t_, ip("239.1.1.1"), 10, 0b110));
  EXPECT_EQ(mdb_lookup(*t_, ip("239.1.1.1"), 10)->ports, 0b110U);
  EXPECT_EQ(mdb_lookup(*t_, ip("239.1.1.1"), 20), nullptr);
  ASSERT_TRUE(neighbor_insert(*t_, ip("10.0.20.2"), mac(9), 20));
  const auto* n = neighbor_lookup(*t_, ip("10.0.20.2"));
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->mac, mac(9));
  EXPECT_EQ(n->vlan, 20);
}

TEST_F(TablesTest, RouteLookupIsLongestPrefixMatch) {
  ASSERT_TRUE(route_insert(*t_, {*Ipv4Prefix::parse("0.0.0.0/0"), 0, 20, ip("10.0.20.254")}));
  ASSERT_TRUE(
      route_insert(*t_, {*Ipv4Prefix::parse("10.0.10.0/24"), route_flag::kConnected, 10, {}}));
  ASSERT_TRUE(route_insert(*t_, {*Ipv4Prefix::parse("10.0.0.0/8"), 0, 20, ip("10.0.20.9")}));
  ASSERT_TRUE(route_insert(*t_, {*Ipv4Prefix::parse("10.0.10.128/25"), 0, 20, ip("10.0.20.7")}));
  EXPECT_EQ(route_lookup(*t_, ip("10.0.10.5"))->vlan, 10);
  EXPECT_EQ(route_lookup(*t_, ip("10.0.10.200"))->nexthop, ip("10.0.20.7"));
  EXPECT_EQ(route_lookup(*t_, ip("10.9.9.9"))->nexthop, ip("10.0.20.9"));
  EXPECT_EQ(route_lookup(*t_, ip("8.8.8.8"))->nexthop, ip("10.0.20.254"));
  // Sorted longest-first.
  for (std::uint32_t i = 1; i < t_->num_routes; ++i) {
    EXPECT_GE(t_->routes[i - 1].prefix.length, t_->routes[i].prefix.length);
  }
}

TEST_F(TablesTest, NoRouteWithoutDefault) {
  ASSERT_TRUE(
      route_insert(*t_, {*Ipv4Prefix::parse("10.0.10.0/24"), route_flag::kConnected, 10, {}}));
  EXPECT_EQ(route_lookup(*t_, ip("192.168.1.1")), nullptr);
}

TEST_F(TablesTest, RouteTableFull) {
  for (std::size_t i = 0; i < kMaxRoutes; ++i) {
    ASSERT_TRUE(route_insert(
        *t_, {Ipv4Prefix{Ipv4Address{static_cast<std::uint32_t>(i) << 8U}, 24}, 0, 1, {}}));
  }
  EXPECT_FALSE(route_insert(*t_, {*Ipv4Prefix::parse("1.2.3.0/24"), 0, 1, {}}));
}

}  // namespace
}  // namespace softswitch::shm
