#pragma once

/// @file
/// Lock-free table publication between the control plane (writer) and data plane (reader).
///
/// The region holds two Tables copies. The data plane reads `tables[active]`; the control plane
/// rebuilds the other copy, then flips `active` and bumps `publish_seq`. Before the control plane
/// may write the copy it just retired, it waits for a grace period (quiescent-state-based
/// reclamation): the data plane either acknowledged the new sequence at the end of a batch, or is
/// idle in poll() holding no table. Only the control plane ever waits; the data plane never
/// blocks on, or takes a lock shared with, the control plane. See docs/design.md.

#include <chrono>
#include <cstdint>

#include "softswitch/shm/region.hpp"

namespace softswitch::shm {

/// Control-plane side. Not thread-safe; one publisher per region.
class TablePublisher {
 public:
  explicit TablePublisher(SharedRegion& region) noexcept : region_(&region) {}

  /// Returns the inactive copy for rebuilding, after waiting up to @p max_wait for the grace
  /// period of the previous publish. Returns nullptr on timeout (the data plane is stuck);
  /// the caller should retry later. The returned copy has stale contents.
  [[nodiscard]] Tables* begin_update(std::chrono::nanoseconds max_wait = std::chrono::milliseconds{
                                         100});

  /// Makes the copy returned by begin_update() live.
  void publish() noexcept;

  /// The live copy (what the data plane currently uses).
  [[nodiscard]] const Tables& live() const noexcept;

  [[nodiscard]] std::uint64_t sequence() const noexcept { return seq_; }
  /// Longest grace-period wait observed, for `show dataplane`.
  [[nodiscard]] std::chrono::nanoseconds max_wait() const noexcept { return max_wait_; }
  [[nodiscard]] std::uint64_t timeouts() const noexcept { return timeouts_; }

  /// True once the data plane has stopped using the copy retired by the last publish.
  [[nodiscard]] bool grace_period_over() const noexcept;

  /// Lock-based publish modes: the caller holds the table lock, which already excludes the
  /// data plane, so begin_update() must not wait for it (it would wait on a blocked reader).
  void set_exclusive(bool exclusive) noexcept { exclusive_ = exclusive; }

 private:
  SharedRegion* region_;
  std::uint64_t seq_ = 0;
  bool updating_ = false;
  bool exclusive_ = false;
  std::chrono::nanoseconds max_wait_{0};
  std::uint64_t timeouts_ = 0;
};

/// Data-plane side. Call enter() at the start of each batch and exit() at its end; call
/// go_idle() before blocking. All three are wait-free.
class TableReader {
 public:
  explicit TableReader(SharedRegion& region) noexcept : region_(&region) {}

  [[nodiscard]] const Tables& enter() noexcept {
    DataPlaneState& dp = region_->dp;
    dp.idle.store(0, std::memory_order_seq_cst);
    seq_ = region_->publish_seq.load(std::memory_order_seq_cst);
    const std::uint32_t active = region_->active.load(std::memory_order_seq_cst);
    return region_->tables[active & 1U];
  }
  void exit() noexcept { region_->dp.ack_seq.store(seq_, std::memory_order_release); }
  void go_idle() noexcept {
    exit();
    region_->dp.idle.store(1, std::memory_order_seq_cst);
  }

 private:
  SharedRegion* region_;
  std::uint64_t seq_ = 0;
};

}  // namespace softswitch::shm
