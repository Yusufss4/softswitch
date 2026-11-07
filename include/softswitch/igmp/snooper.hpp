#pragma once

/// @file
/// IGMP snooping state (RFC 4541): which ports want which multicast groups, and which ports
/// lead to multicast routers. Pure state machine with an explicit clock.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include "softswitch/common/types.hpp"

namespace softswitch::igmp {

using Clock = std::chrono::steady_clock;

struct SnooperConfig {
  /// How long a membership lasts without a new report (RFC 3376: 2 x 125 s + 10 s).
  std::chrono::seconds membership_interval{260};
  /// How long a port stays an mrouter port after the last query seen on it.
  std::chrono::seconds mrouter_timeout{300};
  /// After a leave on a non-fast-leave port: time left for other members to re-report.
  std::chrono::seconds last_member_interval{2};
};

struct GroupKey {
  VlanId vlan = 0;
  Ipv4Address group;
  friend auto operator<=>(const GroupKey&, const GroupKey&) = default;
};

class Snooper {
 public:
  explicit Snooper(SnooperConfig config = {}) : config_(config) {}

  /// A membership report for @p group arrived on @p port. Returns true if membership changed.
  bool on_report(VlanId vlan, Ipv4Address group, PortId port, Clock::time_point now);
  /// A leave for @p group arrived on @p port. With @p fast_leave (a host port: nobody else
  /// can be behind it) the port is removed at once; otherwise it only gets
  /// last_member_interval to be refreshed by another member's report. Returns true if changed.
  bool on_leave(VlanId vlan, Ipv4Address group, PortId port, bool fast_leave,
                Clock::time_point now);
  /// A query arrived on @p port: a multicast router (or querier) is behind it.
  bool on_query(VlanId vlan, PortId port, Clock::time_point now);
  /// Forget everything learned on @p port (link down). Returns true if anything changed.
  bool on_port_down(PortId port);
  /// Forget everything in @p vlan (snooping turned off there).
  bool clear_vlan(VlanId vlan);
  /// Expires memberships and mrouter ports. Returns true if anything changed.
  bool tick(Clock::time_point now);

  [[nodiscard]] PortMask members(VlanId vlan, Ipv4Address group) const;
  [[nodiscard]] PortMask mrouters(VlanId vlan) const;
  /// Group -> port -> expiry, for publishing and `show`.
  [[nodiscard]] const std::map<GroupKey, std::map<PortId, Clock::time_point>>& groups()
      const noexcept {
    return groups_;
  }
  [[nodiscard]] const std::map<VlanId, std::map<PortId, Clock::time_point>>& mrouter_ports()
      const noexcept {
    return mrouters_;
  }
  [[nodiscard]] const SnooperConfig& config() const noexcept { return config_; }
  void set_config(const SnooperConfig& config) noexcept { config_ = config; }

 private:
  SnooperConfig config_;
  std::map<GroupKey, std::map<PortId, Clock::time_point>> groups_;
  std::map<VlanId, std::map<PortId, Clock::time_point>> mrouters_;
};

}  // namespace softswitch::igmp
