#include <gtest/gtest.h>

#include "softswitch/mac_table.hpp"

namespace {

using softswitch::MacAddress;
using softswitch::MacTable;
using softswitch::PortId;
using namespace std::chrono_literals;

constexpr MacAddress kMacA{0x02, 0, 0, 0, 0, 1};
constexpr MacAddress kMacB{0x02, 0, 0, 0, 0, 2};

TEST(MacTable, UnknownMacMisses) {
  const MacTable table;
  EXPECT_FALSE(table.lookup(kMacA, MacTable::Clock::now()).has_value());
}

TEST(MacTable, LearnAndLookup) {
  MacTable table;
  const auto now = MacTable::Clock::now();
  table.learn(kMacA, 3, now);
  EXPECT_EQ(table.lookup(kMacA, now), PortId{3});
  EXPECT_FALSE(table.lookup(kMacB, now).has_value());
}

TEST(MacTable, MacMoveUpdatesPort) {
  MacTable table;
  const auto now = MacTable::Clock::now();
  table.learn(kMacA, 1, now);
  table.learn(kMacA, 2, now + 1s);
  EXPECT_EQ(table.lookup(kMacA, now + 1s), PortId{2});
  EXPECT_EQ(table.size(), 1U);
}

TEST(MacTable, AgesOut) {
  MacTable table{10s};
  const auto now = MacTable::Clock::now();
  table.learn(kMacA, 1, now);
  EXPECT_EQ(table.lookup(kMacA, now + 10s), PortId{1});  // exactly at the limit: still valid
  EXPECT_FALSE(table.lookup(kMacA, now + 11s).has_value());
  EXPECT_EQ(table.age_out(now + 11s), 1U);
  EXPECT_EQ(table.size(), 0U);
}

TEST(MacTable, RelearnRefreshesAging) {
  MacTable table{10s};
  const auto now = MacTable::Clock::now();
  table.learn(kMacA, 1, now);
  table.learn(kMacA, 1, now + 8s);
  EXPECT_EQ(table.age_out(now + 15s), 0U);
  EXPECT_EQ(table.lookup(kMacA, now + 15s), PortId{1});
}

}  // namespace
