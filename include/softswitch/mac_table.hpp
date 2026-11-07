#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace softswitch {

using MacAddress = std::array<std::uint8_t, 6>;
using PortId = std::uint16_t;

struct MacHash {
  std::size_t operator()(const MacAddress& m) const noexcept;
};

// L2 forwarding database: learns source MAC -> ingress port, with aging.
class MacTable {
 public:
  using Clock = std::chrono::steady_clock;

  explicit MacTable(std::chrono::seconds aging = std::chrono::seconds{300});

  void learn(const MacAddress& mac, PortId port, Clock::time_point now = Clock::now());
  std::optional<PortId> lookup(const MacAddress& mac, Clock::time_point now = Clock::now()) const;
  std::size_t age_out(Clock::time_point now = Clock::now());
  std::size_t size() const noexcept { return table_.size(); }

 private:
  struct Entry {
    PortId port;
    Clock::time_point last_seen;
  };
  std::chrono::seconds aging_;
  std::unordered_map<MacAddress, Entry, MacHash> table_;
};

}  // namespace softswitch
