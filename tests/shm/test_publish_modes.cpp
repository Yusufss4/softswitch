// Publish modes: the lock-based modes exist only for the priority-inversion demo, but they must
// still be correct, and the QSBR default must keep waiting for the data plane.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "softswitch/shm/publisher.hpp"
#include "softswitch/shm/region.hpp"

namespace softswitch::shm {
namespace {

class PublishModeTest : public ::testing::Test {
 protected:
  std::unique_ptr<SharedRegion> region_ = std::make_unique<SharedRegion>();

  /// A data plane that is attached and inside a batch that started before the last publish.
  void busy_dataplane() {
    region_->dp.attached.store(1);
    region_->dp.idle.store(0);
    region_->dp.ack_seq.store(0);
  }
};

TEST_F(PublishModeTest, QsbrWaitsForTheDataPlaneAndTimesOut) {
  TablePublisher publisher(*region_);
  ASSERT_NE(publisher.begin_update(), nullptr);
  publisher.publish();  // seq 1; the data plane has not acknowledged it
  busy_dataplane();
  EXPECT_EQ(publisher.begin_update(std::chrono::milliseconds{2}), nullptr);
  EXPECT_EQ(publisher.timeouts(), 1U);
}

TEST_F(PublishModeTest, ExclusivePublisherDoesNotWait) {
  // Under the table lock the data plane cannot be reading, so waiting for its acknowledgement
  // would only deadlock against a reader blocked on that same lock.
  TablePublisher publisher(*region_);
  publisher.set_exclusive(true);
  ASSERT_NE(publisher.begin_update(), nullptr);
  publisher.publish();
  busy_dataplane();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_NE(publisher.begin_update(std::chrono::seconds{1}), nullptr);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{100});
  EXPECT_EQ(publisher.timeouts(), 0U);
}

TEST(PublishModeNames, RoundTrip) {
  for (const auto mode : {PublishMode::kQsbr, PublishMode::kMutex, PublishMode::kMutexPi}) {
    const auto parsed = parse_publish_mode(to_string(mode));
    ASSERT_TRUE(parsed.has_value()) << to_string(mode);
    EXPECT_EQ(*parsed, mode);
  }
  EXPECT_FALSE(parse_publish_mode("rcu").has_value());
  EXPECT_FALSE(parse_publish_mode("").has_value());
}

}  // namespace
}  // namespace softswitch::shm
