#include "softswitch/igmp/snooper.hpp"

namespace softswitch::igmp {
namespace {

template <typename Map>
bool erase_port(Map& m, PortId port) {
  bool changed = false;
  for (auto it = m.begin(); it != m.end();) {
    changed = it->second.erase(port) != 0 || changed;
    it = it->second.empty() ? m.erase(it) : std::next(it);
  }
  return changed;
}

template <typename Map>
bool expire(Map& m, Clock::time_point now) {
  bool changed = false;
  for (auto it = m.begin(); it != m.end();) {
    changed =
        std::erase_if(it->second, [&](const auto& kv) { return kv.second <= now; }) != 0 || changed;
    it = it->second.empty() ? m.erase(it) : std::next(it);
  }
  return changed;
}

}  // namespace

bool Snooper::on_report(VlanId vlan, Ipv4Address group, PortId port, Clock::time_point now) {
  if (!group.is_multicast() || group.is_local_multicast()) {
    return false;  // 224.0.0.x is always flooded; never tracked
  }
  auto& ports = groups_[GroupKey{vlan, group}];
  const bool is_new = !ports.contains(port);
  ports[port] = now + config_.membership_interval;
  return is_new;
}

bool Snooper::on_leave(VlanId vlan, Ipv4Address group, PortId port, bool fast_leave,
                       Clock::time_point now) {
  const auto it = groups_.find(GroupKey{vlan, group});
  if (it == groups_.end() || !it->second.contains(port)) {
    return false;
  }
  if (fast_leave) {
    it->second.erase(port);
    if (it->second.empty()) {
      groups_.erase(it);
    }
    return true;
  }
  auto& expiry = it->second[port];
  expiry = std::min(expiry, now + config_.last_member_interval);
  return false;  // membership unchanged for now; tick() removes it if nobody re-reports
}

bool Snooper::on_query(VlanId vlan, PortId port, Clock::time_point now) {
  auto& ports = mrouters_[vlan];
  const bool is_new = !ports.contains(port);
  ports[port] = now + config_.mrouter_timeout;
  return is_new;
}

bool Snooper::on_port_down(PortId port) {
  const bool a = erase_port(groups_, port);
  const bool b = erase_port(mrouters_, port);
  return a || b;
}

bool Snooper::clear_vlan(VlanId vlan) {
  const auto n = std::erase_if(groups_, [&](const auto& kv) { return kv.first.vlan == vlan; });
  return mrouters_.erase(vlan) != 0 || n != 0;
}

bool Snooper::tick(Clock::time_point now) {
  const bool a = expire(groups_, now);
  const bool b = expire(mrouters_, now);
  return a || b;
}

PortMask Snooper::members(VlanId vlan, Ipv4Address group) const {
  PortMask mask = 0;
  if (const auto it = groups_.find(GroupKey{vlan, group}); it != groups_.end()) {
    for (const auto& [port, expiry] : it->second) {
      mask |= port_bit(port);
    }
  }
  return mask;
}

PortMask Snooper::mrouters(VlanId vlan) const {
  PortMask mask = 0;
  if (const auto it = mrouters_.find(vlan); it != mrouters_.end()) {
    for (const auto& [port, expiry] : it->second) {
      mask |= port_bit(port);
    }
  }
  return mask;
}

}  // namespace softswitch::igmp
