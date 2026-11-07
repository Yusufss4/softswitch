#include <gtest/gtest.h>

#include "softswitch/l2/fdb.hpp"

namespace softswitch::l2 {
namespace {

using namespace std::chrono_literals;
using Clock = Fdb::Clock;

MacAddress mac(std::uint64_t n) { return MacAddress::from_u64(0x020000000000ULL | n); }

TEST(Fdb, LearnNewRefreshAndMove) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  EXPECT_EQ(fdb.learn(mac(1), 10, 2, t0), LearnResult::kNew);
  EXPECT_EQ(fdb.learn(mac(1), 10, 2, t0 + 1s), LearnResult::kRefreshed);
  EXPECT_EQ(fdb.learn(mac(1), 10, 3, t0 + 2s), LearnResult::kMoved);
  EXPECT_EQ(fdb.lookup(mac(1), 10), PortId{3});
  EXPECT_EQ(fdb.moves(), 1U);
  EXPECT_EQ(fdb.size(), 1U);
}

TEST(Fdb, IndependentVlanLearning) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  EXPECT_EQ(fdb.learn(mac(1), 10, 2, t0), LearnResult::kNew);
  EXPECT_EQ(fdb.learn(mac(1), 20, 5, t0), LearnResult::kNew);
  EXPECT_EQ(fdb.lookup(mac(1), 10), PortId{2});
  EXPECT_EQ(fdb.lookup(mac(1), 20), PortId{5});
  EXPECT_FALSE(fdb.lookup(mac(1), 30).has_value());
}

TEST(Fdb, AgingRespectsTouches) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  (void)fdb.learn(mac(1), 10, 1, t0);
  (void)fdb.learn(mac(2), 10, 1, t0);
  fdb.touch(mac(2), 10, t0 + 8s);
  EXPECT_EQ(fdb.age_out(t0 + 10s, 10s), 0U);  // exactly at the limit: kept
  EXPECT_EQ(fdb.age_out(t0 + 11s, 10s), 1U);
  EXPECT_FALSE(fdb.lookup(mac(1), 10).has_value());
  EXPECT_TRUE(fdb.lookup(mac(2), 10).has_value());
}

TEST(Fdb, StaticEntriesNeverAgeMoveOrFlush) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  ASSERT_TRUE(fdb.add_static(mac(1), 10, 4));
  EXPECT_EQ(fdb.learn(mac(1), 10, 2, t0), LearnResult::kStaticConflict);
  EXPECT_EQ(fdb.lookup(mac(1), 10), PortId{4});
  EXPECT_EQ(fdb.age_out(t0 + 1000s, 10s), 0U);
  EXPECT_EQ(fdb.flush(std::nullopt, std::nullopt), 0U);
  fdb.clear_static();
  EXPECT_EQ(fdb.size(), 0U);
}

TEST(Fdb, FlushByPortAndVlan) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  (void)fdb.learn(mac(1), 10, 1, t0);
  (void)fdb.learn(mac(2), 10, 2, t0);
  (void)fdb.learn(mac(3), 20, 1, t0);
  EXPECT_EQ(fdb.flush(PortId{1}, VlanId{10}), 1U);
  EXPECT_EQ(fdb.flush(PortId{1}, std::nullopt), 1U);
  EXPECT_EQ(fdb.flush(std::nullopt, VlanId{10}), 1U);
  EXPECT_EQ(fdb.size(), 0U);
}

TEST(Fdb, CapacityLimit) {
  Fdb fdb(2);
  const auto t0 = Clock::now();
  EXPECT_EQ(fdb.learn(mac(1), 1, 0, t0), LearnResult::kNew);
  EXPECT_EQ(fdb.learn(mac(2), 1, 0, t0), LearnResult::kNew);
  EXPECT_EQ(fdb.learn(mac(3), 1, 0, t0), LearnResult::kFull);
  EXPECT_EQ(fdb.full_drops(), 1U);
  EXPECT_EQ(fdb.learn(mac(1), 1, 1, t0), LearnResult::kMoved);  // existing entries still move
}

TEST(Fdb, RemoveIfNotKeepsStatics) {
  Fdb fdb(16);
  const auto t0 = Clock::now();
  (void)fdb.learn(mac(1), 10, 1, t0);
  ASSERT_TRUE(fdb.add_static(mac(2), 10, 1));
  EXPECT_EQ(fdb.remove_if_not([](const FdbEntry&) { return false; }), 1U);
  EXPECT_TRUE(fdb.lookup(mac(2), 10).has_value());
}

}  // namespace
}  // namespace softswitch::l2
