// Runtime pieces of the benchmark and inversion work: the shared table lock and TX mode names.
#include <gtest/gtest.h>
#include <pthread.h>

#include <cerrno>
#include <memory>
#include <string>
#include <thread>

#include "softswitch/dp/af_packet_port.hpp"
#include "softswitch/shm/region.hpp"
#include "softswitch/shm/region_mapping.hpp"

namespace softswitch {
namespace {

TEST(TableLock, QsbrModeLeavesNoLock) {
  auto region = std::make_unique<shm::SharedRegion>();
  shm::set_publish_mode(*region, shm::PublishMode::kQsbr);
  EXPECT_EQ(region->publish_mode, shm::PublishMode::kQsbr);
}

class TableLockModes : public ::testing::TestWithParam<shm::PublishMode> {};

TEST_P(TableLockModes, ExcludesASecondThread) {
  auto region = std::make_unique<shm::SharedRegion>();
  shm::set_publish_mode(*region, GetParam());
  EXPECT_EQ(region->publish_mode, GetParam());
  ASSERT_EQ(::pthread_mutex_lock(&region->table_lock), 0);
  int other = 0;
  std::thread([&] { other = ::pthread_mutex_trylock(&region->table_lock); }).join();
  EXPECT_EQ(other, EBUSY);
  ASSERT_EQ(::pthread_mutex_unlock(&region->table_lock), 0);
  std::thread([&] {
    other = ::pthread_mutex_trylock(&region->table_lock);
    if (other == 0) {
      (void)::pthread_mutex_unlock(&region->table_lock);
    }
  }).join();
  EXPECT_EQ(other, 0);
  ::pthread_mutex_destroy(&region->table_lock);
}

std::string lock_mode_name(const ::testing::TestParamInfo<shm::PublishMode>& param) {
  return param.param == shm::PublishMode::kMutex ? "Mutex" : "MutexPi";
}

INSTANTIATE_TEST_SUITE_P(LockModes, TableLockModes,
                         ::testing::Values(shm::PublishMode::kMutex, shm::PublishMode::kMutexPi),
                         lock_mode_name);

TEST(TxMode, ParsesEveryMode) {
  EXPECT_EQ(dp::parse_tx_mode("sendmmsg"), dp::TxMode::kSendmmsg);
  EXPECT_EQ(dp::parse_tx_mode("ring"), dp::TxMode::kRing);
  EXPECT_EQ(dp::parse_tx_mode("sendto"), dp::TxMode::kSendto);
  EXPECT_FALSE(dp::parse_tx_mode("xdp").has_value());
  EXPECT_FALSE(dp::parse_tx_mode("").has_value());
}

TEST(TxMode, DefaultIsSendmmsg) {
  // Chosen by measurement (docs/benchmark.md): 29 % higher no-drop rate than the TX ring.
  EXPECT_EQ(dp::AfPacketOptions{}.tx_mode, dp::TxMode::kSendmmsg);
}

}  // namespace
}  // namespace softswitch
