#pragma once

/// @file
/// The switch configuration model and the configuration commands that change it.
///
/// Configuration is a plain value (`SwitchConfig`). Commands are single lines in a
/// Cisco-like syntax; `render_running_config()` prints a config as exactly those lines, so
/// `show running-config` output can be replayed as a startup config.

#include <bitset>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/shm/tables.hpp"

namespace softswitch::cp {

using VlanSet = std::bitset<kVlanTableSize>;

[[nodiscard]] VlanSet all_vlans() noexcept;

struct PortConfig {
  std::string name;
  std::string description;
  bool shutdown = false;
  shm::PortMode mode = shm::PortMode::kAccess;
  VlanId access_vlan = 1;
  VlanId native_vlan = 1;
  VlanSet trunk_allowed = all_vlans();

  friend bool operator==(const PortConfig&, const PortConfig&) = default;
};

struct VlanConfig {
  std::string name;
  friend bool operator==(const VlanConfig&, const VlanConfig&) = default;
};

struct StaticMac {
  MacAddress mac;
  VlanId vlan = 0;
  std::string port;
  friend bool operator==(const StaticMac&, const StaticMac&) = default;
};

inline constexpr std::uint32_t kDefaultMacAgingSeconds = 300;

struct SwitchConfig {
  std::string hostname;
  std::vector<PortConfig> ports;
  std::map<VlanId, VlanConfig> vlans{{VlanId{1}, VlanConfig{"default"}}};
  std::uint32_t mac_aging_seconds = kDefaultMacAgingSeconds;
  std::vector<StaticMac> static_macs;
  std::uint32_t trace_sample = 0;

  /// Builds a default config with one access port in VLAN 1 per name.
  [[nodiscard]] static SwitchConfig with_ports(const std::string& hostname,
                                               const std::vector<std::string>& port_names);
  [[nodiscard]] std::optional<PortId> port_index(std::string_view name) const noexcept;
  [[nodiscard]] bool vlan_exists(VlanId vid) const noexcept { return vlans.contains(vid); }

  friend bool operator==(const SwitchConfig&, const SwitchConfig&) = default;
};

struct CommandResult {
  bool ok = true;
  bool changed = false;
  std::string message;

  [[nodiscard]] static CommandResult success(std::string msg = {}) {
    return {true, true, std::move(msg)};
  }
  [[nodiscard]] static CommandResult unchanged(std::string msg = {}) {
    return {true, false, std::move(msg)};
  }
  [[nodiscard]] static CommandResult error(std::string msg) {
    return {false, false, std::move(msg)};
  }
};

/// Splits a command line on whitespace; '!' or '#' starts a comment.
[[nodiscard]] std::vector<std::string> tokenize(std::string_view line);

/// Parses "10,20,30-40", "all" or "none".
[[nodiscard]] std::optional<VlanSet> parse_vlan_list(std::string_view text);
/// The inverse of parse_vlan_list (ranges compressed, "all"/"none" when applicable).
[[nodiscard]] std::string format_vlan_list(const VlanSet& vlans);

/// True if @p tokens is a configuration command this module understands.
[[nodiscard]] bool is_config_command(const std::vector<std::string>& tokens) noexcept;
/// Applies one configuration command.
[[nodiscard]] CommandResult apply_config(SwitchConfig& config,
                                         const std::vector<std::string>& tokens);

/// The configuration as replayable command lines.
[[nodiscard]] std::string render_running_config(const SwitchConfig& config);

/// True if @p port is administratively up and a member of existing VLAN @p vlan.
[[nodiscard]] bool port_in_vlan(const SwitchConfig& config, PortId port, VlanId vlan) noexcept;

/// Writes ports (name, admin state, mode, PVID) and VLAN membership into @p tables.
/// Operational state (oper up, STP state, ifindex, MACs) is left to the caller.
void apply_l2_config(const SwitchConfig& config, shm::Tables& tables);

}  // namespace softswitch::cp
