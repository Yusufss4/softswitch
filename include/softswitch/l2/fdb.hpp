#pragma once

/// @file
/// The control plane's authoritative L2 forwarding database (MAC address table).
///
/// Keyed by (MAC, VLAN) as in IEEE 802.1Q independent VLAN learning. The data plane never
/// writes it: it reports misses and moves on the learn ring, and marks hits in a bitmap that
/// the control plane folds back in with touch() before aging.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "softswitch/common/types.hpp"

namespace softswitch::l2 {

struct FdbEntry {
  MacAddress mac;
  VlanId vlan = 0;
  PortId port = kInvalidPort;
  bool is_static = false;
  std::chrono::steady_clock::time_point last_seen;
};

enum class LearnResult : std::uint8_t {
  kNew,             ///< First time this (MAC, VLAN) was seen.
  kMoved,           ///< Known, but now seen on a different port (station move).
  kRefreshed,       ///< Known on this port; only the age was reset.
  kStaticConflict,  ///< A static entry pins this MAC elsewhere; not learned.
  kFull,            ///< Table at capacity; not learned.
};

class Fdb {
 public:
  using Clock = std::chrono::steady_clock;

  explicit Fdb(std::size_t capacity) noexcept : capacity_(capacity) {}

  /// Records that @p mac was seen as a source on @p port in @p vlan.
  LearnResult learn(const MacAddress& mac, VlanId vlan, PortId port, Clock::time_point now);
  /// Adds or replaces a static entry. Returns false if the table is full.
  bool add_static(const MacAddress& mac, VlanId vlan, PortId port);
  /// Removes every static entry (used before re-applying them from the config).
  void clear_static();
  /// Resets the age of a dynamic entry (the data plane saw it as a source).
  void touch(const MacAddress& mac, VlanId vlan, Clock::time_point now) noexcept;

  [[nodiscard]] std::optional<PortId> lookup(const MacAddress& mac, VlanId vlan) const noexcept;
  [[nodiscard]] const FdbEntry* find(const MacAddress& mac, VlanId vlan) const noexcept;

  /// Removes dynamic entries not seen for longer than @p aging. Returns the count removed.
  std::size_t age_out(Clock::time_point now, std::chrono::seconds aging);
  /// Removes dynamic entries matching the filters (nullopt = any). Returns the count removed.
  std::size_t flush(std::optional<PortId> port, std::optional<VlanId> vlan);
  /// Removes dynamic entries for which @p keep returns false.
  template <typename Pred>
  std::size_t remove_if_not(Pred keep) {
    return std::erase_if(entries_,
                         [&](const auto& kv) { return !kv.second.is_static && !keep(kv.second); });
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] const std::unordered_map<std::uint64_t, FdbEntry>& entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] std::uint64_t moves() const noexcept { return moves_; }
  [[nodiscard]] std::uint64_t full_drops() const noexcept { return full_drops_; }

  [[nodiscard]] static constexpr std::uint64_t key(const MacAddress& mac, VlanId vlan) noexcept {
    return (mac.to_u64() << 16U) | vlan;
  }

 private:
  std::size_t capacity_;
  std::unordered_map<std::uint64_t, FdbEntry> entries_;
  std::uint64_t moves_ = 0;
  std::uint64_t full_drops_ = 0;
};

}  // namespace softswitch::l2
