#include "softswitch/cp/config.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace softswitch::cp {
namespace {

using Tokens = std::vector<std::string>;

std::optional<std::uint32_t> parse_uint(std::string_view text, std::uint32_t max) noexcept {
  std::uint32_t value = 0;
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), last, value);
  if (text.empty() || ec != std::errc{} || ptr != last || value > max) {
    return std::nullopt;
  }
  return value;
}

std::optional<VlanId> parse_vlan_id(std::string_view text) noexcept {
  const auto v = parse_uint(text, kMaxVlanId);
  if (!v || !valid_vlan(*v)) {
    return std::nullopt;
  }
  return static_cast<VlanId>(*v);
}

std::string join(const Tokens& tokens, std::size_t from) {
  std::string out;
  for (std::size_t i = from; i < tokens.size(); ++i) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += tokens[i];
  }
  return out;
}

CommandResult bad_vlan(std::string_view text) {
  return CommandResult::error("invalid VLAN id '" + std::string(text) + "' (1-4094)");
}

// ---- vlan ID [name NAME] / no vlan ID -----------------------------------------------------

CommandResult cmd_vlan(SwitchConfig& c, const Tokens& t, bool negate) {
  if (t.size() < 2) {
    return CommandResult::error("usage: vlan ID [name NAME]");
  }
  const auto vid = parse_vlan_id(t[1]);
  if (!vid) {
    return bad_vlan(t[1]);
  }
  if (negate) {
    if (*vid == 1) {
      return CommandResult::error("VLAN 1 cannot be deleted");
    }
    return c.vlans.erase(*vid) != 0 ? CommandResult::success() : CommandResult::unchanged();
  }
  // Validate before touching the config: a rejected command must not leave a VLAN behind.
  const bool named = t.size() >= 4 && t[2] == "name";
  if (!named && t.size() != 2) {
    return CommandResult::error("usage: vlan ID [name NAME]");
  }
  auto& vlan = c.vlans[*vid];
  if (named) {
    vlan.name = join(t, 3);
  }
  if (vlan.name.empty()) {
    vlan.name = "VLAN" + std::to_string(*vid);
  }
  return CommandResult::success();
}

// ---- interface PORT ... ---------------------------------------------------------------------

CommandResult cmd_switchport(PortConfig& port, const Tokens& t, std::size_t i) {
  // t[i] is the word after "switchport".
  if (i + 1 < t.size() && t[i] == "mode") {
    if (t[i + 1] == "access") {
      port.mode = shm::PortMode::kAccess;
    } else if (t[i + 1] == "trunk") {
      port.mode = shm::PortMode::kTrunk;
    } else {
      return CommandResult::error("switchport mode must be 'access' or 'trunk'");
    }
    return CommandResult::success();
  }
  if (i + 2 < t.size() && t[i] == "access" && t[i + 1] == "vlan") {
    const auto vid = parse_vlan_id(t[i + 2]);
    if (!vid) {
      return bad_vlan(t[i + 2]);
    }
    port.access_vlan = *vid;
    return CommandResult::success();
  }
  if (i + 3 < t.size() && t[i] == "trunk" && t[i + 1] == "native" && t[i + 2] == "vlan") {
    const auto vid = parse_vlan_id(t[i + 3]);
    if (!vid) {
      return bad_vlan(t[i + 3]);
    }
    port.native_vlan = *vid;
    return CommandResult::success();
  }
  if (i + 3 < t.size() && t[i] == "trunk" && t[i + 1] == "allowed" && t[i + 2] == "vlan") {
    std::size_t j = i + 3;
    std::string op = "set";
    if (j < t.size() && (t[j] == "add" || t[j] == "remove")) {
      op = t[j];
      ++j;
    }
    if (j >= t.size()) {
      return CommandResult::error("usage: switchport trunk allowed vlan [add|remove] LIST");
    }
    const auto list = parse_vlan_list(t[j]);
    if (!list) {
      return CommandResult::error("invalid VLAN list '" + t[j] + "'");
    }
    if (op == "add") {
      port.trunk_allowed |= *list;
    } else if (op == "remove") {
      port.trunk_allowed &= ~*list;
    } else {
      port.trunk_allowed = *list;
    }
    return CommandResult::success();
  }
  return CommandResult::error("unknown switchport command");
}

CommandResult cmd_interface(SwitchConfig& c, const Tokens& t) {
  if (t.size() < 3) {
    return CommandResult::error(
        "usage: interface PORT <description|shutdown|no shutdown|switchport ...>");
  }
  const auto index = c.port_index(t[1]);
  if (!index) {
    return CommandResult::error("no such interface '" + t[1] + "'");
  }
  PortConfig& port = c.ports[*index];
  if (t[2] == "description") {
    port.description = join(t, 3);
    return CommandResult::success();
  }
  if (t[2] == "shutdown" && t.size() == 3) {
    port.shutdown = true;
    return CommandResult::success();
  }
  if (t[2] == "no" && t.size() == 4 && t[3] == "shutdown") {
    port.shutdown = false;
    return CommandResult::success();
  }
  if (t[2] == "no" && t.size() == 4 && t[3] == "description") {
    port.description.clear();
    return CommandResult::success();
  }
  if (t[2] == "switchport") {
    return cmd_switchport(port, t, 3);
  }
  return CommandResult::error("unknown interface command '" + t[2] + "'");
}

CommandResult cmd_trace(SwitchConfig& c, const Tokens& t) {
  if (t.size() == 3 && t[1] == "sample") {
    const auto n = parse_uint(t[2], 1'000'000);
    if (!n) {
      return CommandResult::error("trace sample N: record 1 in N packets (0 = off)");
    }
    c.trace_sample = *n;
    return CommandResult::success();
  }
  return CommandResult::error("usage: trace sample N");
}

CommandResult cmd_mac(SwitchConfig& c, const Tokens& t) {
  if (t.size() == 4 && t[1] == "address-table" && t[2] == "aging-time") {
    const auto secs = parse_uint(t[3], 1'000'000);
    if (!secs || *secs < 10) {
      return CommandResult::error("aging time must be 10..1000000 seconds");
    }
    c.mac_aging_seconds = *secs;
    return CommandResult::success();
  }
  return CommandResult::error("usage: mac address-table aging-time SECONDS");
}

}  // namespace

VlanSet all_vlans() noexcept {
  VlanSet set;
  set.set();
  set.reset(0);
  set.reset(kVlanTableSize - 1);
  return set;
}

SwitchConfig SwitchConfig::with_ports(const std::string& hostname,
                                      const std::vector<std::string>& port_names) {
  SwitchConfig config;
  config.hostname = hostname;
  for (const auto& name : port_names) {
    PortConfig port;
    port.name = name;
    config.ports.push_back(port);
  }
  return config;
}

std::optional<PortId> SwitchConfig::port_index(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (ports[i].name == name) {
      return static_cast<PortId>(i);
    }
  }
  return std::nullopt;
}

std::vector<std::string> tokenize(std::string_view line) {
  const auto comment = line.find_first_of("!#");
  line = line.substr(0, comment);
  std::vector<std::string> tokens;
  std::istringstream in{std::string(line)};
  std::string word;
  while (in >> word) {
    tokens.push_back(word);
  }
  return tokens;
}

std::optional<VlanSet> parse_vlan_list(std::string_view text) {
  if (text == "all") {
    return all_vlans();
  }
  if (text == "none") {
    return VlanSet{};
  }
  VlanSet set;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto item = text.substr(0, comma);
    const auto dash = item.find('-');
    const auto lo = parse_vlan_id(item.substr(0, dash));
    const auto hi = dash == std::string_view::npos ? lo : parse_vlan_id(item.substr(dash + 1));
    if (!lo || !hi || *lo > *hi) {
      return std::nullopt;
    }
    for (unsigned v = *lo; v <= *hi; ++v) {
      set.set(v);
    }
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
  }
  return set;
}

std::string format_vlan_list(const VlanSet& vlans) {
  if (vlans == all_vlans()) {
    return "all";
  }
  if (vlans.none()) {
    return "none";
  }
  std::string out;
  for (unsigned v = kMinVlanId; v <= kMaxVlanId; ++v) {
    if (!vlans.test(v)) {
      continue;
    }
    unsigned end = v;
    while (end + 1 <= kMaxVlanId && vlans.test(end + 1)) {
      ++end;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += std::to_string(v);
    if (end != v) {
      out += "-" + std::to_string(end);
    }
    v = end;
  }
  return out;
}

bool is_config_command(const std::vector<std::string>& t) noexcept {
  if (t.empty()) {
    return false;
  }
  const std::string& w = t[0] == "no" && t.size() > 1 ? t[1] : t[0];
  return w == "hostname" || w == "vlan" || w == "interface" || w == "trace" || w == "mac";
}

CommandResult apply_config(SwitchConfig& c, const std::vector<std::string>& t) {
  if (t.empty()) {
    return CommandResult::unchanged();
  }
  if (t[0] == "hostname") {
    if (t.size() != 2) {
      return CommandResult::error("usage: hostname NAME");
    }
    c.hostname = t[1];
    return CommandResult::success();
  }
  if (t[0] == "vlan") {
    return cmd_vlan(c, t, false);
  }
  if (t[0] == "no" && t.size() > 1 && t[1] == "vlan") {
    return cmd_vlan(c, Tokens(t.begin() + 1, t.end()), true);
  }
  if (t[0] == "interface") {
    return cmd_interface(c, t);
  }
  if (t[0] == "trace") {
    return cmd_trace(c, t);
  }
  if (t[0] == "mac") {
    return cmd_mac(c, t);
  }
  return CommandResult::error("unknown command '" + t[0] + "'");
}

std::string render_running_config(const SwitchConfig& c) {
  std::ostringstream out;
  out << "! softswitch running configuration\n";
  if (!c.hostname.empty()) {
    out << "hostname " << c.hostname << "\n";
  }
  for (const auto& [vid, vlan] : c.vlans) {
    if (vid == 1 && vlan.name == "default") {
      continue;
    }
    out << "vlan " << vid;
    if (vlan.name != "VLAN" + std::to_string(vid)) {
      out << " name " << vlan.name;
    }
    out << "\n";
  }
  if (c.mac_aging_seconds != kDefaultMacAgingSeconds) {
    out << "mac address-table aging-time " << c.mac_aging_seconds << "\n";
  }
  if (c.trace_sample != 0) {
    out << "trace sample " << c.trace_sample << "\n";
  }
  const PortConfig defaults;
  for (const auto& p : c.ports) {
    const std::string prefix = "interface " + p.name + " ";
    if (!p.description.empty()) {
      out << prefix << "description " << p.description << "\n";
    }
    if (p.mode == shm::PortMode::kTrunk) {
      out << prefix << "switchport mode trunk\n";
    }
    if (p.access_vlan != defaults.access_vlan) {
      out << prefix << "switchport access vlan " << p.access_vlan << "\n";
    }
    if (p.native_vlan != defaults.native_vlan) {
      out << prefix << "switchport trunk native vlan " << p.native_vlan << "\n";
    }
    if (p.trunk_allowed != defaults.trunk_allowed) {
      out << prefix << "switchport trunk allowed vlan " << format_vlan_list(p.trunk_allowed)
          << "\n";
    }
    if (p.shutdown) {
      out << prefix << "shutdown\n";
    }
  }
  return out.str();
}

void apply_l2_config(const SwitchConfig& c, shm::Tables& t) {
  t.num_ports = static_cast<std::uint32_t>(std::min<std::size_t>(c.ports.size(), kMaxPorts));
  t.trace_sample = c.trace_sample;
  t.vlans.fill(shm::VlanEntry{});
  for (const auto& [vid, vlan] : c.vlans) {
    (void)vlan;
    t.vlans[vid].flags = shm::vlan_flag::kActive;
  }
  for (std::uint32_t idx = 0; idx < t.num_ports; ++idx) {
    const auto i = static_cast<PortId>(idx);
    const PortConfig& p = c.ports[i];
    shm::PortEntry& e = t.ports[i];
    e.name.fill('\0');
    std::copy_n(p.name.begin(), std::min(p.name.size(), shm::kIfNameLen - 1), e.name.begin());
    e.mode = p.mode;
    e.flags = static_cast<std::uint8_t>(p.shutdown ? (e.flags & ~shm::port_flag::kAdminUp)
                                                   : (e.flags | shm::port_flag::kAdminUp));
    const PortMask bit = port_bit(i);
    if (p.mode == shm::PortMode::kAccess) {
      // An access port in a VLAN that doesn't exist is inactive (no membership, drops all).
      const bool active = c.vlan_exists(p.access_vlan);
      e.pvid = active ? p.access_vlan : VlanId{0};
      if (active) {
        t.vlans[p.access_vlan].members |= bit;
        t.vlans[p.access_vlan].untagged |= bit;
      }
      continue;
    }
    // Trunk: member of every existing allowed VLAN; the native VLAN is untagged.
    for (const auto& [vid, vlan] : c.vlans) {
      (void)vlan;
      if (p.trunk_allowed.test(vid)) {
        t.vlans[vid].members |= bit;
      }
    }
    const bool native_ok = c.vlan_exists(p.native_vlan) && p.trunk_allowed.test(p.native_vlan);
    e.pvid = native_ok ? p.native_vlan : VlanId{0};
    if (native_ok) {
      t.vlans[p.native_vlan].untagged |= bit;
    }
  }
}

}  // namespace softswitch::cp
