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

/// interface vlanN ip address A.B.C.D/N | interface vlanN no ip address
CommandResult cmd_svi(SwitchConfig& c, const Tokens& t) {
  const auto vid = parse_vlan_id(std::string_view(t[1]).substr(4));
  if (!vid) {
    return bad_vlan(std::string_view(t[1]).substr(4));
  }
  if (t.size() == 5 && t[2] == "ip" && t[3] == "address") {
    const auto prefix = Ipv4Prefix::parse(t[4]);
    if (!prefix || prefix->length == 0 || prefix->length > 30 ||
        prefix->address == prefix->network()) {
      return CommandResult::error("need a host address with a /1-/30 prefix, e.g. 10.0.10.1/24");
    }
    for (const auto& [other, addr] : c.svis) {
      if (other != *vid && (addr.contains(prefix->address) || prefix->contains(addr.address))) {
        return CommandResult::error("overlaps the subnet of interface vlan" +
                                    std::to_string(other));
      }
    }
    c.svis[*vid] = *prefix;
    return CommandResult::success();
  }
  if (t.size() == 5 && t[2] == "no" && t[3] == "ip" && t[4] == "address") {
    return c.svis.erase(*vid) != 0 ? CommandResult::success() : CommandResult::unchanged();
  }
  return CommandResult::error(
      "usage: interface vlanN ip address A.B.C.D/N | interface vlanN no ip address");
}

CommandResult cmd_interface(SwitchConfig& c, const Tokens& t) {
  if (t.size() >= 3 && t[1].starts_with("vlan") && !c.port_index(t[1])) {
    return cmd_svi(c, t);
  }
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
  if (t[2] == "spanning-tree" && t.size() == 4 && t[3] == "portfast") {
    port.stp_edge = true;
    return CommandResult::success();
  }
  if (t[2] == "no" && t.size() == 5 && t[3] == "spanning-tree" && t[4] == "portfast") {
    port.stp_edge = false;
    return CommandResult::success();
  }
  if (t[2] == "spanning-tree" && t.size() == 5 && t[3] == "cost") {
    const auto cost = parse_uint(t[4], 200'000'000);
    if (!cost || *cost == 0) {
      return CommandResult::error("path cost must be 1..200000000");
    }
    port.stp_cost = *cost;
    return CommandResult::success();
  }
  if (t[2] == "spanning-tree" && t.size() == 5 && t[3] == "port-priority") {
    const auto prio = parse_uint(t[4], 240);
    if (!prio || *prio % 16 != 0) {
      return CommandResult::error("port priority must be 0..240 in steps of 16");
    }
    port.stp_priority = static_cast<std::uint8_t>(*prio);
    return CommandResult::success();
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

CommandResult cmd_mac(SwitchConfig& c, const Tokens& t, bool negate) {
  if (!negate && t.size() == 4 && t[1] == "address-table" && t[2] == "aging-time") {
    const auto secs = parse_uint(t[3], 1'000'000);
    if (!secs || *secs < 10) {
      return CommandResult::error("aging time must be 10..1000000 seconds");
    }
    c.mac_aging_seconds = *secs;
    return CommandResult::success();
  }
  // mac address-table static MAC vlan ID interface PORT
  // no mac address-table static MAC vlan ID
  const std::size_t need = negate ? 6 : 8;
  if (t.size() == need && t[1] == "address-table" && t[2] == "static" && t[4] == "vlan" &&
      (negate || t[6] == "interface")) {
    const auto mac = MacAddress::parse(t[3]);
    if (!mac || mac->is_multicast() || mac->is_zero()) {
      return CommandResult::error("invalid unicast MAC address '" + t[3] + "'");
    }
    const auto vid = parse_vlan_id(t[5]);
    if (!vid) {
      return bad_vlan(t[5]);
    }
    const auto same = [&](const StaticMac& m) { return m.mac == *mac && m.vlan == *vid; };
    if (negate) {
      return std::erase_if(c.static_macs, same) != 0 ? CommandResult::success()
                                                     : CommandResult::unchanged();
    }
    if (!c.port_index(t[7])) {
      return CommandResult::error("no such interface '" + t[7] + "'");
    }
    std::erase_if(c.static_macs, same);
    c.static_macs.push_back(StaticMac{*mac, *vid, t[7]});
    return CommandResult::success();
  }
  return CommandResult::error(
      "usage: mac address-table aging-time SECONDS | mac address-table static MAC vlan ID "
      "interface PORT");
}

CommandResult cmd_spanning_tree(SwitchConfig& c, const Tokens& t, bool negate) {
  if (t.size() == 1) {
    c.stp_enabled = !negate;
    return CommandResult::success();
  }
  if (negate || t.size() != 3) {
    return CommandResult::error(
        "usage: [no] spanning-tree | spanning-tree priority|hello-time|max-age|forward-time N");
  }
  const auto value = parse_uint(t[2], 65535);
  if (!value) {
    return CommandResult::error("invalid number '" + t[2] + "'");
  }
  if (t[1] == "priority") {
    if (*value > 61440 || *value % 4096 != 0) {
      return CommandResult::error("bridge priority must be 0..61440 in steps of 4096");
    }
    c.stp_priority = static_cast<std::uint16_t>(*value);
  } else if (t[1] == "hello-time" && *value >= 1 && *value <= 10) {
    c.stp_hello = *value;
  } else if (t[1] == "max-age" && *value >= 6 && *value <= 40) {
    c.stp_max_age = *value;
  } else if (t[1] == "forward-time" && *value >= 4 && *value <= 30) {
    c.stp_forward_delay = *value;
  } else {
    return CommandResult::error("out of range (hello-time 1-10, max-age 6-40, forward-time 4-30)");
  }
  return CommandResult::success();
}

// ip igmp snooping [vlan ID | querier | query-interval N | fast-leave]  (and the "no" forms)
CommandResult cmd_ip(SwitchConfig& c, const Tokens& t, bool negate) {
  if (t.size() == 2 && t[1] == "routing") {
    c.ip_routing = !negate;
    return CommandResult::success();
  }
  if (t.size() == 4 && t[1] == "route") {
    auto prefix = Ipv4Prefix::parse(t[2]);
    const auto nexthop = Ipv4Address::parse(t[3]);
    if (!prefix || !nexthop) {
      return CommandResult::error("usage: [no] ip route A.B.C.D/N NEXTHOP");
    }
    prefix->address = prefix->network();
    const SwitchConfig::StaticRoute route{*prefix, *nexthop};
    if (negate) {
      return std::erase(c.static_routes, route) != 0 ? CommandResult::success()
                                                     : CommandResult::unchanged();
    }
    if (std::find(c.static_routes.begin(), c.static_routes.end(), route) != c.static_routes.end()) {
      return CommandResult::unchanged();
    }
    if (c.static_routes.size() >= 48) {
      return CommandResult::error("too many static routes (max 48)");
    }
    c.static_routes.push_back(route);
    return CommandResult::success();
  }
  if (!negate && t.size() == 4 && t[1] == "arp" && t[2] == "timeout") {
    const auto secs = parse_uint(t[3], 86400);
    if (!secs || *secs < 10) {
      return CommandResult::error("ARP timeout must be 10..86400 seconds");
    }
    c.arp_timeout = *secs;
    return CommandResult::success();
  }
  if (t.size() >= 3 && t[1] == "igmp" && t[2] == "snooping") {
    if (t.size() == 3) {
      c.igmp_snooping = !negate;
      return CommandResult::success();
    }
    if (t.size() == 5 && t[3] == "vlan") {
      const auto vid = parse_vlan_id(t[4]);
      if (!vid) {
        return bad_vlan(t[4]);
      }
      if (negate) {
        c.igmp_disabled_vlans.insert(*vid);
      } else {
        c.igmp_disabled_vlans.erase(*vid);
      }
      return CommandResult::success();
    }
    if (t.size() == 4 && t[3] == "querier") {
      c.igmp_querier = !negate;
      return CommandResult::success();
    }
    if (t.size() == 4 && t[3] == "fast-leave") {
      c.igmp_fast_leave = !negate;
      return CommandResult::success();
    }
    if (!negate && t.size() == 5 && t[3] == "query-interval") {
      const auto secs = parse_uint(t[4], 3600);
      if (!secs || *secs < 10) {
        return CommandResult::error("query interval must be 10..3600 seconds");
      }
      c.igmp_query_interval = *secs;
      return CommandResult::success();
    }
  }
  return CommandResult::error(
      "usage: [no] ip igmp snooping [vlan ID | querier | fast-leave] | ip igmp snooping "
      "query-interval N");
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
  unsigned v = kMinVlanId;
  while (v <= kMaxVlanId) {
    if (!vlans.test(v)) {
      ++v;
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
    v = end + 1;
  }
  return out;
}

bool is_config_command(const std::vector<std::string>& t) noexcept {
  if (t.empty()) {
    return false;
  }
  const std::string& w = t[0] == "no" && t.size() > 1 ? t[1] : t[0];
  return w == "hostname" || w == "vlan" || w == "interface" || w == "trace" || w == "mac" ||
         w == "spanning-tree" || w == "ip";
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
  if (t[0] == "ip") {
    return cmd_ip(c, t, false);
  }
  if (t[0] == "no" && t.size() > 1 && t[1] == "ip") {
    return cmd_ip(c, Tokens(t.begin() + 1, t.end()), true);
  }
  if (t[0] == "spanning-tree") {
    return cmd_spanning_tree(c, t, false);
  }
  if (t[0] == "no" && t.size() > 1 && t[1] == "spanning-tree") {
    return cmd_spanning_tree(c, Tokens(t.begin() + 1, t.end()), true);
  }
  if (t[0] == "mac") {
    return cmd_mac(c, t, false);
  }
  if (t[0] == "no" && t.size() > 1 && t[1] == "mac") {
    return cmd_mac(c, Tokens(t.begin() + 1, t.end()), true);
  }
  return CommandResult::error("unknown command '" + t[0] + "'");
}

namespace {

// Each render_* helper below appends one section of the running configuration. A setting is
// printed only when it differs from its default, as the command that would set it, so the output
// replays through apply_config() into the same SwitchConfig.

/// `vlan ID [name NAME]` for every VLAN except the built-in default VLAN 1. The name is left
/// out when it is the automatic one (`VLAN<id>`).
void render_vlans(std::ostream& out, const SwitchConfig& c) {
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
}

/// The bridge-wide RSTP settings: on/off, bridge priority and the three protocol timers.
void render_stp(std::ostream& out, const SwitchConfig& c, const SwitchConfig& defaults) {
  if (!c.stp_enabled) {
    out << "no spanning-tree\n";
  }
  if (c.stp_priority != defaults.stp_priority) {
    out << "spanning-tree priority " << c.stp_priority << "\n";
  }
  if (c.stp_hello != defaults.stp_hello) {
    out << "spanning-tree hello-time " << c.stp_hello << "\n";
  }
  if (c.stp_max_age != defaults.stp_max_age) {
    out << "spanning-tree max-age " << c.stp_max_age << "\n";
  }
  if (c.stp_forward_delay != defaults.stp_forward_delay) {
    out << "spanning-tree forward-time " << c.stp_forward_delay << "\n";
  }
}

/// IGMP snooping: global on/off, the VLANs it is disabled in, the querier and fast-leave.
void render_igmp(std::ostream& out, const SwitchConfig& c, const SwitchConfig& defaults) {
  if (!c.igmp_snooping) {
    out << "no ip igmp snooping\n";
  }
  for (const VlanId v : c.igmp_disabled_vlans) {
    out << "no ip igmp snooping vlan " << v << "\n";
  }
  if (c.igmp_querier) {
    out << "ip igmp snooping querier\n";
  }
  if (c.igmp_query_interval != defaults.igmp_query_interval) {
    out << "ip igmp snooping query-interval " << c.igmp_query_interval << "\n";
  }
  if (!c.igmp_fast_leave) {
    out << "no ip igmp snooping fast-leave\n";
  }
}

/// Layer 3: the ARP timeout, SVI addresses, whether routing is on, and the static routes.
void render_l3(std::ostream& out, const SwitchConfig& c, const SwitchConfig& defaults) {
  if (c.arp_timeout != defaults.arp_timeout) {
    out << "ip arp timeout " << c.arp_timeout << "\n";
  }
  for (const auto& [vid, prefix] : c.svis) {
    out << "interface vlan" << vid << " ip address " << prefix.to_string() << "\n";
  }
  if (!c.ip_routing) {
    out << "no ip routing\n";
  }
  for (const auto& r : c.static_routes) {
    out << "ip route " << r.prefix.to_string() << " " << r.nexthop.to_string() << "\n";
  }
}

/// One port's settings, each as a one-line `interface NAME ...` command.
void render_port(std::ostream& out, const PortConfig& p, const PortConfig& defaults) {
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
    out << prefix << "switchport trunk allowed vlan " << format_vlan_list(p.trunk_allowed) << "\n";
  }
  if (p.stp_edge) {
    out << prefix << "spanning-tree portfast\n";
  }
  if (p.stp_cost != defaults.stp_cost) {
    out << prefix << "spanning-tree cost " << p.stp_cost << "\n";
  }
  if (p.stp_priority != defaults.stp_priority) {
    out << prefix << "spanning-tree port-priority " << unsigned{p.stp_priority} << "\n";
  }
  if (p.shutdown) {
    out << prefix << "shutdown\n";
  }
}

}  // namespace

std::string render_running_config(const SwitchConfig& c) {
  const SwitchConfig defaults;
  std::ostringstream out;
  out << "! softswitch running configuration\n";
  if (!c.hostname.empty()) {
    out << "hostname " << c.hostname << "\n";
  }
  render_vlans(out, c);
  if (c.mac_aging_seconds != kDefaultMacAgingSeconds) {
    out << "mac address-table aging-time " << c.mac_aging_seconds << "\n";
  }
  render_stp(out, c, defaults);
  render_igmp(out, c, defaults);
  render_l3(out, c, defaults);
  for (const auto& m : c.static_macs) {
    out << "mac address-table static " << m.mac.to_string() << " vlan " << m.vlan << " interface "
        << m.port << "\n";
  }
  if (c.trace_sample != 0) {
    out << "trace sample " << c.trace_sample << "\n";
  }
  const PortConfig port_defaults;
  for (const auto& p : c.ports) {
    render_port(out, p, port_defaults);
  }
  return out.str();
}

bool port_in_vlan(const SwitchConfig& c, PortId port, VlanId vlan) noexcept {
  if (port >= c.ports.size() || !c.vlan_exists(vlan)) {
    return false;
  }
  const PortConfig& p = c.ports[port];
  if (p.shutdown) {
    return false;
  }
  return p.mode == shm::PortMode::kAccess ? p.access_vlan == vlan : p.trunk_allowed.test(vlan);
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
