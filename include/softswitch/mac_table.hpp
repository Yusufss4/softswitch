#pragma once

/// @file
/// L2 forwarding database (FDB): maps a learned source MAC address to its ingress port.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace softswitch {

/// 48-bit Ethernet MAC address in network byte order.
using MacAddress = std::array<std::uint8_t, 6>;
/// Logical switch port number.
using PortId = std::uint16_t;

/// Hash for MacAddress keys.
struct MacHash {
  std::size_t operator()(const MacAddress& mac) const noexcept;
};

/// Learns source MAC -> ingress port and expires entries not seen for the aging time.
///
/// Every operation takes the current time explicitly, so tests and fuzzers can use a
/// simulated clock.
class MacTable {
 public:
  using Clock = std::chrono::steady_clock;

  /// Default aging time from IEEE 802.1Q (300 s).
  static constexpr std::chrono::seconds kDefaultAging{300};

  explicit MacTable(std::chrono::seconds aging = kDefaultAging);

  /// Records that @p mac was seen on @p port at @p now. Overwrites earlier entries (MAC move).
  void learn(const MacAddress& mac, PortId port, Clock::time_point now = Clock::now());

  /// Returns the port @p mac was learned on, or nullopt if unknown or aged out.
  [[nodiscard]] std::optional<PortId> lookup(const MacAddress& mac,
                                             Clock::time_point now = Clock::now()) const;

  /// Removes entries older than the aging time. Returns how many were removed.
  std::size_t age_out(Clock::time_point now = Clock::now());

  /// Number of entries, including expired ones not yet removed by age_out().
  [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }

 private:
  struct Entry {
    PortId port{};
    Clock::time_point last_seen;
  };

  [[nodiscard]] bool expired(const Entry& entry, Clock::time_point now) const noexcept {
    return now - entry.last_seen > aging_;
  }

  std::chrono::seconds aging_;
  std::unordered_map<MacAddress, Entry, MacHash> table_;
};

}  // namespace softswitch
