#include <gtest/gtest.h>

#include "softswitch/mac_table.hpp"

using namespace softswitch;
using namespace std::chrono_literals;

TEST(MacTable, LearnAndLookup) {
  MacTable t;
  MacAddress a{0x02, 0, 0, 0, 0, 1};
  auto now = MacTable::Clock::now();
  EXPECT_FALSE(t.lookup(a, now));
  t.learn(a, 3, now);
  EXPECT_EQ(t.lookup(a, now), PortId{3});
}

TEST(MacTable, AgesOut) {
  MacTable t{10s};
  MacAddress a{0x02, 0, 0, 0, 0, 1};
  auto now = MacTable::Clock::now();
  t.learn(a, 1, now);
  EXPECT_FALSE(t.lookup(a, now + 11s));
  EXPECT_EQ(t.age_out(now + 11s), 1u);
  EXPECT_EQ(t.size(), 0u);
}
