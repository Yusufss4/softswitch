#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "softswitch/shm/publisher.hpp"

namespace softswitch::shm {
namespace {

using namespace std::chrono_literals;

// Stamp a recognisable pattern derived from one value into a sample of slots spread over the
// whole Tables copy, so a reader can detect a torn (partially rewritten) table.
constexpr std::size_t kSamples = 64;

void stamp(Tables& t, std::uint32_t value) {
  t.trace_sample = value;
  for (std::size_t i = 0; i < kSamples; ++i) {
    t.fdb[i * (kFdbSlots / kSamples)].port = static_cast<PortId>(value & 0xffffU);
    t.vlans[i * (kVlanTableSize / kSamples)].members = value;
  }
  t.num_routes = value;
}

bool consistent(const Tables& t) {
  const std::uint32_t value = t.trace_sample;
  for (std::size_t i = 0; i < kSamples; ++i) {
    if (t.fdb[i * (kFdbSlots / kSamples)].port != static_cast<PortId>(value & 0xffffU) ||
        t.vlans[i * (kVlanTableSize / kSamples)].members != value) {
      return false;
    }
  }
  return t.num_routes == value;
}

class PublisherTest : public ::testing::Test {
 protected:
  std::unique_ptr<SharedRegion> region_ = std::make_unique<SharedRegion>();
};

TEST_F(PublisherTest, PublishFlipsActiveCopy) {
  TablePublisher pub(*region_);
  Tables* staged = pub.begin_update();
  ASSERT_NE(staged, nullptr);
  EXPECT_EQ(staged, &region_->tables[1]);
  stamp(*staged, 42);
  pub.publish();
  EXPECT_EQ(region_->active.load(), 1U);
  EXPECT_EQ(region_->publish_seq.load(), 1U);
  EXPECT_EQ(pub.live().trace_sample, 42U);
  EXPECT_EQ(pub.live().generation, 1U);

  TableReader reader(*region_);
  region_->dp.attached = 1;
  EXPECT_EQ(reader.enter().trace_sample, 42U);
  reader.exit();
}

TEST_F(PublisherTest, WaitsForBusyReaderAndTimesOut) {
  TablePublisher pub(*region_);
  TableReader reader(*region_);
  region_->dp.attached = 1;

  // Reader starts a batch on copy 0 (nothing published yet) and keeps it.
  const Tables& held = reader.enter();
  EXPECT_EQ(&held, &region_->tables[0]);

  // First update writes copy 1, which nobody reads: no wait.
  ASSERT_NE(pub.begin_update(10ms), nullptr);
  pub.publish();  // seq 1, copy 1 live; copy 0 retired but still held by the reader

  EXPECT_FALSE(pub.grace_period_over());
  EXPECT_EQ(pub.begin_update(5ms), nullptr);  // would overwrite the reader's copy
  EXPECT_EQ(pub.timeouts(), 1U);

  reader.exit();  // acks seq 0 only: still not enough
  EXPECT_FALSE(pub.grace_period_over());
  EXPECT_EQ(&reader.enter(), &region_->tables[1]);  // the next batch picks up seq 1
  reader.exit();
  EXPECT_TRUE(pub.grace_period_over());
  EXPECT_NE(pub.begin_update(5ms), nullptr);
}

TEST_F(PublisherTest, IdleReaderDoesNotHoldGracePeriod) {
  TablePublisher pub(*region_);
  TableReader reader(*region_);
  region_->dp.attached = 1;
  ASSERT_NE(pub.begin_update(), nullptr);
  pub.publish();
  (void)reader.enter();
  reader.go_idle();
  ASSERT_NE(pub.begin_update(), nullptr);
  pub.publish();
  EXPECT_TRUE(pub.grace_period_over());  // idle reader holds no table
}

TEST_F(PublisherTest, DetachedReaderDoesNotHoldGracePeriod) {
  TablePublisher pub(*region_);
  TableReader reader(*region_);
  region_->dp.attached = 1;
  (void)reader.enter();  // reads copy 0 ... and the data plane crashes mid-batch
  ASSERT_NE(pub.begin_update(), nullptr);
  pub.publish();
  EXPECT_FALSE(pub.grace_period_over());
  region_->dp.attached = 0;
  EXPECT_TRUE(pub.grace_period_over());
}

// The real thing: a reader thread hammering enter/exit (and occasionally going idle) while the
// publisher rewrites tables as fast as it can. The reader must never observe a torn copy.
// Run under TSan in CI: the happens-before edges come only from the publisher's atomics.
TEST_F(PublisherTest, ConcurrentReaderNeverSeesTornTables) {
  region_->dp.attached = 1;
  stamp(region_->tables[0], 0);  // both copies start out consistent
  stamp(region_->tables[1], 0);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> torn{0};
  std::thread reader_thread([&] {
    TableReader reader(*region_);
    std::uint64_t n = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const Tables& t = reader.enter();
      if (!consistent(t)) {
        torn.fetch_add(1);
      }
      if ((++n % 16) == 0) {
        reader.go_idle();
      } else {
        reader.exit();
      }
    }
    reader.go_idle();
    reads.store(n);
  });

  TablePublisher pub(*region_);
  std::uint32_t published = 0;
  const auto deadline = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < deadline) {
    Tables* t = pub.begin_update(1s);
    ASSERT_NE(t, nullptr);
    stamp(*t, ++published);
    pub.publish();
  }
  stop.store(true);
  reader_thread.join();
  EXPECT_EQ(torn.load(), 0U);
  EXPECT_GT(published, 10U);
  EXPECT_GT(reads.load(), 10U);
}

}  // namespace
}  // namespace softswitch::shm
