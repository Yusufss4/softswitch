#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "softswitch/shm/spsc_ring.hpp"

namespace softswitch::shm {
namespace {

TEST(SpscRing, PushPopInOrder) {
  SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.empty());
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_EQ(ring.size(), 5U);
  for (int i = 0; i < 5; ++i) {
    int v = -1;
    ASSERT_TRUE(ring.try_pop(v));
    EXPECT_EQ(v, i);
  }
  int v = 0;
  EXPECT_FALSE(ring.try_pop(v));
}

TEST(SpscRing, FullRingCountsDrops) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_FALSE(ring.try_push(100));
  EXPECT_EQ(ring.drops(), 2U);
  int v = 0;
  ASSERT_TRUE(ring.try_pop(v));
  EXPECT_TRUE(ring.try_push(4));  // space again after one pop
}

TEST(SpscRing, WrapsAround) {
  SpscRing<std::uint32_t, 4> ring;
  for (std::uint32_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(ring.try_push(i));
    std::uint32_t v = 0;
    ASSERT_TRUE(ring.try_pop(v));
    ASSERT_EQ(v, i);
  }
}

TEST(SpscRing, ReserveCommitWritesInPlace) {
  struct Big {
    std::uint32_t seq;
    std::array<std::uint8_t, 1500> data;
  };
  auto ring = std::make_unique<SpscRing<Big, 4>>();
  Big* slot = ring->reserve();
  ASSERT_NE(slot, nullptr);
  slot->seq = 7;
  slot->data[1499] = 0xAB;
  EXPECT_TRUE(ring->empty());  // not visible before commit
  ring->commit();
  const Big* front = ring->front();
  ASSERT_NE(front, nullptr);
  EXPECT_EQ(front->seq, 7U);
  EXPECT_EQ(front->data[1499], 0xAB);
  ring->pop();
  EXPECT_EQ(ring->front(), nullptr);
}

// Producer and consumer on different threads (run under TSan in CI). Every element that was
// accepted must arrive exactly once and in order; rejected pushes are accounted as drops.
TEST(SpscRing, TwoThreadStressPreservesOrder) {
  constexpr std::uint64_t kCount = 200000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 64>>();
  std::uint64_t accepted = 0;
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kCount; ++i) {
      if (ring->try_push(i)) {
        ++accepted;
      }
    }
    while (!ring->try_push(0)) {  // sentinel
    }
  });
  std::uint64_t received = 0;
  std::uint64_t last = 0;
  bool ordered = true;
  for (;;) {
    std::uint64_t v = 0;
    if (!ring->try_pop(v)) {
      continue;
    }
    if (v == 0) {
      break;
    }
    ordered = ordered && v > last;
    last = v;
    ++received;
  }
  producer.join();
  EXPECT_TRUE(ordered);
  EXPECT_EQ(received, accepted);
  EXPECT_GE(ring->drops(), kCount - accepted);  // the sentinel retries also count as drops
}

}  // namespace
}  // namespace softswitch::shm
