#include "softswitch/mac_table.hpp"

namespace softswitch {

std::size_t MacHash::operator()(const MacAddress& m) const noexcept {
  std::uint64_t v = 0;
  for (auto b : m) v = (v << 8) | b;
  return std::hash<std::uint64_t>{}(v);
}

MacTable::MacTable(std::chrono::seconds aging) : aging_(aging) {}

void MacTable::learn(const MacAddress& mac, PortId port, Clock::time_point now) {
  table_[mac] = Entry{port, now};
}

std::optional<PortId> MacTable::lookup(const MacAddress& mac, Clock::time_point now) const {
  auto it = table_.find(mac);
  if (it == table_.end() || now - it->second.last_seen > aging_) return std::nullopt;
  return it->second.port;
}

std::size_t MacTable::age_out(Clock::time_point now) {
  return std::erase_if(table_, [&](const auto& kv) { return now - kv.second.last_seen > aging_; });
}

}  // namespace softswitch
