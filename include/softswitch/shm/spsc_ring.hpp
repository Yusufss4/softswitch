#pragma once

/// @file
/// Lock-free single-producer/single-consumer ring that lives in shared memory.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace softswitch::shm {

inline constexpr std::size_t kCacheLine = 64;

/// Bounded SPSC queue of trivially copyable @p T. Exactly one producer and one consumer, which
/// may be in different processes. Neither side ever blocks: a full ring rejects the push and
/// counts a drop, which is how the data plane stays independent of the control plane.
///
/// Each side caches the other side's index, so the common case touches only its own cache line.
template <typename T, std::size_t N>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "ring slots are copied across processes");
  static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "needs address-free atomics");

 public:
  static constexpr std::size_t kCapacity = N;

  // ---- producer side -------------------------------------------------------------------
  /// Returns a slot to fill in place, or nullptr if the ring is full (the drop is counted).
  /// Must be followed by commit() before the next reserve().
  [[nodiscard]] T* reserve() noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - producer_cached_tail_ >= N) {
      producer_cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - producer_cached_tail_ >= N) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
    }
    return &slots_[head & kMask];
  }
  /// Publishes the slot returned by reserve().
  void commit() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  /// Copies @p value in. Returns false (and counts a drop) if full.
  bool try_push(const T& value) noexcept {
    T* slot = reserve();
    if (slot == nullptr) {
      return false;
    }
    *slot = value;
    commit();
    return true;
  }

  // ---- consumer side -------------------------------------------------------------------
  /// Oldest element, or nullptr if empty. Must be followed by pop() once done with it.
  [[nodiscard]] const T* front() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == consumer_cached_head_) {
      consumer_cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == consumer_cached_head_) {
        return nullptr;
      }
    }
    return &slots_[tail & kMask];
  }
  void pop() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  bool try_pop(T& out) noexcept {
    const T* slot = front();
    if (slot == nullptr) {
      return false;
    }
    out = *slot;
    pop();
    return true;
  }

  // ---- either side (approximate) -------------------------------------------------------
  [[nodiscard]] std::size_t size() const noexcept {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::uint64_t drops() const noexcept {
    return drops_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr std::uint64_t kMask = N - 1;

  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t producer_cached_tail_ = 0;
  std::atomic<std::uint64_t> drops_{0};
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t consumer_cached_head_ = 0;
  alignas(kCacheLine) std::array<T, N> slots_{};
};

}  // namespace softswitch::shm
