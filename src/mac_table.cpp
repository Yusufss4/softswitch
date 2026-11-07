#include "softswitch/mac_table.hpp"

#include <functional>
#include <numeric>

namespace softswitch {

std::size_t MacHash::operator()(const MacAddress& mac) const noexcept {
  const std::uint64_t value =
      std::accumulate(mac.begin(), mac.end(), std::uint64_t{0},
                      [](std::uint64_t acc, std::uint8_t byte) { return (acc << 8U) | byte; });
  return std::hash<std::uint64_t>{}(value);
}

MacTable::MacTable(std::chrono::seconds aging) : aging_(aging) {}

void MacTable::learn(const MacAddress& mac, PortId port, Clock::time_point now) {
  table_.insert_or_assign(mac, Entry{port, now});
}

std::optional<PortId> MacTable::lookup(const MacAddress& mac, Clock::time_point now) const {
  const auto it = table_.find(mac);
  if (it == table_.end() || expired(it->second, now)) {
    return std::nullopt;
  }
  return it->second.port;
}

std::size_t MacTable::age_out(Clock::time_point now) {
  return std::erase_if(table_, [&](const auto& kv) { return expired(kv.second, now); });
}

}  // namespace softswitch
