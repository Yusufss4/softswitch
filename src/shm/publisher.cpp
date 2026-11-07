#include "softswitch/shm/publisher.hpp"

#include <thread>

namespace softswitch::shm {

bool TablePublisher::grace_period_over() const noexcept {
  const DataPlaneState& dp = region_->dp;
  return seq_ == 0 || dp.attached.load(std::memory_order_seq_cst) == 0 ||
         dp.idle.load(std::memory_order_seq_cst) != 0 ||
         dp.ack_seq.load(std::memory_order_acquire) >= seq_;
}

Tables* TablePublisher::begin_update(std::chrono::nanoseconds max_wait) {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  int spins = 0;
  while (!grace_period_over()) {
    const auto waited = Clock::now() - start;
    if (waited >= max_wait) {
      ++timeouts_;
      return nullptr;
    }
    // The data plane finishes a batch in microseconds; spin briefly, then yield the CPU.
    if (++spins > 64) {
      std::this_thread::sleep_for(std::chrono::microseconds{20});
    }
  }
  const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  if (waited > max_wait_) {
    max_wait_ = waited;
  }
  updating_ = true;
  const std::uint32_t active = region_->active.load(std::memory_order_relaxed);
  return &region_->tables[(active + 1U) & 1U];
}

void TablePublisher::publish() noexcept {
  if (!updating_) {
    return;
  }
  updating_ = false;
  const std::uint32_t next = (region_->active.load(std::memory_order_relaxed) + 1U) & 1U;
  region_->tables[next].generation = seq_ + 1;
  region_->active.store(next, std::memory_order_seq_cst);
  region_->publish_seq.store(++seq_, std::memory_order_seq_cst);
}

const Tables& TablePublisher::live() const noexcept {
  return region_->tables[region_->active.load(std::memory_order_relaxed) & 1U];
}

}  // namespace softswitch::shm
