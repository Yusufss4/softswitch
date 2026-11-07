#include "softswitch/l2/fdb.hpp"

namespace softswitch::l2 {

LearnResult Fdb::learn(const MacAddress& mac, VlanId vlan, PortId port, Clock::time_point now) {
  const auto it = entries_.find(key(mac, vlan));
  if (it != entries_.end()) {
    FdbEntry& e = it->second;
    if (e.is_static) {
      return e.port == port ? LearnResult::kRefreshed : LearnResult::kStaticConflict;
    }
    e.last_seen = now;
    if (e.port == port) {
      return LearnResult::kRefreshed;
    }
    e.port = port;
    ++moves_;
    return LearnResult::kMoved;
  }
  if (entries_.size() >= capacity_) {
    ++full_drops_;
    return LearnResult::kFull;
  }
  entries_.emplace(key(mac, vlan), FdbEntry{mac, vlan, port, false, now});
  return LearnResult::kNew;
}

bool Fdb::add_static(const MacAddress& mac, VlanId vlan, PortId port) {
  const auto k = key(mac, vlan);
  if (!entries_.contains(k) && entries_.size() >= capacity_) {
    return false;
  }
  entries_[k] = FdbEntry{mac, vlan, port, true, {}};
  return true;
}

void Fdb::clear_static() {
  std::erase_if(entries_, [](const auto& kv) { return kv.second.is_static; });
}

void Fdb::touch(const MacAddress& mac, VlanId vlan, Clock::time_point now) noexcept {
  const auto it = entries_.find(key(mac, vlan));
  if (it != entries_.end() && !it->second.is_static) {
    it->second.last_seen = now;
  }
}

std::optional<PortId> Fdb::lookup(const MacAddress& mac, VlanId vlan) const noexcept {
  const FdbEntry* e = find(mac, vlan);
  return e != nullptr ? std::optional<PortId>{e->port} : std::nullopt;
}

const FdbEntry* Fdb::find(const MacAddress& mac, VlanId vlan) const noexcept {
  const auto it = entries_.find(key(mac, vlan));
  return it != entries_.end() ? &it->second : nullptr;
}

std::size_t Fdb::age_out(Clock::time_point now, std::chrono::seconds aging) {
  return std::erase_if(entries_, [&](const auto& kv) {
    return !kv.second.is_static && now - kv.second.last_seen > aging;
  });
}

std::size_t Fdb::flush(std::optional<PortId> port, std::optional<VlanId> vlan) {
  return std::erase_if(entries_, [&](const auto& kv) {
    const FdbEntry& e = kv.second;
    return !e.is_static && (!port || e.port == *port) && (!vlan || e.vlan == *vlan);
  });
}

}  // namespace softswitch::l2
