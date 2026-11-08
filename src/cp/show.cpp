// `show` commands of softswitchd.
#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>

#include "softswitch/cp/control_plane.hpp"
#include "softswitch/cp/text_table.hpp"
#include "softswitch/sys/realtime.hpp"
#include "softswitch/version.hpp"

namespace softswitch::cp {
namespace {

std::string u64(std::uint64_t v) { return std::to_string(v); }
std::uint64_t get(const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); }

constexpr const char* kHelp =
    "show version | running-config | interfaces | vlan | counters | dataplane\n"
    "show mac address-table [vlan ID] [interface PORT]\n"
    "show spanning-tree\n"
    "show ip igmp snooping | show ip interface | show ip route | show arp\n"
    "clear arp\n"
    "clear mac address-table [dynamic] [vlan ID] [interface PORT]\n"
    "vlan ID [name NAME] | no vlan ID\n"
    "interface PORT description TEXT | shutdown | no shutdown\n"
    "interface PORT switchport mode access|trunk\n"
    "interface PORT switchport access vlan ID\n"
    "interface PORT switchport trunk allowed vlan [add|remove] LIST\n"
    "interface PORT switchport trunk native vlan ID\n"
    "mac address-table aging-time SECONDS\n"
    "mac address-table static MAC vlan ID interface PORT | no mac address-table static MAC vlan "
    "ID\n"
    "[no] spanning-tree | spanning-tree priority|hello-time|max-age|forward-time N\n"
    "interface PORT spanning-tree portfast | cost N | port-priority N\n"
    "[no] ip igmp snooping [vlan ID | querier | fast-leave] | ip igmp snooping query-interval N\n"
    "interface vlanN ip address A.B.C.D/N | interface vlanN no ip address | ip arp timeout N\n"
    "[no] ip routing | [no] ip route A.B.C.D/N NEXTHOP\n"
    "trace sample N\n"
    "hostname NAME\n";

}  // namespace

std::string ControlPlane::show(const std::vector<std::string>& t) const {
  const std::string what = t.size() > 1 ? t[1] : "help";
  if (what == "version") {
    return show_version();
  }
  if (what == "running-config") {
    return render_running_config(config_);
  }
  if (what == "interfaces") {
    return show_interfaces();
  }
  if (what == "vlan") {
    return show_vlan();
  }
  if (what == "counters") {
    return show_counters();
  }
  if (what == "dataplane") {
    return show_dataplane();
  }
  if (what == "mac") {
    return show_mac(t);
  }
  if (what == "spanning-tree") {
    return show_spanning_tree();
  }
  if (what == "ip" && t.size() > 2 && t[2] == "igmp") {
    return show_igmp();
  }
  if (what == "ip" && t.size() > 2 && t[2] == "interface") {
    return show_ip_interface();
  }
  if (what == "ip" && t.size() > 2 && t[2] == "route") {
    return show_ip_route();
  }
  if (what == "arp" || (what == "ip" && t.size() > 2 && t[2] == "arp")) {
    return show_arp();
  }
  if (what == "help") {
    return kHelp;
  }
  return "% unknown show command '" + what + "'\n";
}

std::string ControlPlane::show_version() const {
  std::ostringstream out;
  out << "softswitch " << kVersion << "\n"
      << "hostname " << config_.hostname << ", bridge MAC " << bridge_mac_.to_string() << "\n"
      << "data plane " << (dataplane_attached() ? "attached" : "detached") << ", "
      << config_.ports.size() << " ports\n";
  return out.str();
}

std::string ControlPlane::show_interfaces() const {
  const shm::Tables& live = publisher_->live();
  TextTable table(
      {"Port", "Admin", "Oper", "Mode", "PVID", "STP", "RX", "TX", "Drops", "Description"});
  for (std::size_t i = 0; i < config_.ports.size(); ++i) {
    const auto& p = config_.ports[i];
    const auto& e = live.ports[i];
    const auto& c = region_->dp.ports[i];
    table.add_row({p.name, p.shutdown ? "down" : "up", runtime_[i].oper_up ? "up" : "down",
                   p.mode == shm::PortMode::kTrunk ? "trunk" : "access",
                   e.pvid == 0 ? "-" : std::to_string(e.pvid), shm::to_string(e.stp_state),
                   u64(get(c.rx_packets)), u64(get(c.tx_packets)), u64(get(c.drops)),
                   p.description});
  }
  return table.render();
}

std::string ControlPlane::show_vlan() const {
  const shm::Tables& live = publisher_->live();
  const auto ports_in = [this](PortMask mask) {
    std::string out;
    for (std::size_t i = 0; i < config_.ports.size(); ++i) {
      if (has_port(mask, static_cast<PortId>(i))) {
        out += (out.empty() ? "" : ",") + config_.ports[i].name;
      }
    }
    return out.empty() ? std::string("-") : out;
  };
  TextTable table({"VLAN", "Name", "Untagged", "Tagged"});
  for (const auto& [vid, vlan] : config_.vlans) {
    const auto& e = live.vlans[vid];
    table.add_row(
        {std::to_string(vid), vlan.name, ports_in(e.untagged), ports_in(e.members & ~e.untagged)});
  }
  return table.render();
}

std::string ControlPlane::show_counters() const {
  TextTable ports(
      {"Port", "RX packets", "RX bytes", "TX packets", "TX bytes", "Drops", "RX ring full"});
  for (std::size_t i = 0; i < config_.ports.size(); ++i) {
    const auto& c = region_->dp.ports[i];
    ports.add_row({config_.ports[i].name, u64(get(c.rx_packets)), u64(get(c.rx_bytes)),
                   u64(get(c.tx_packets)), u64(get(c.tx_bytes)), u64(get(c.drops)),
                   u64(get(c.rx_ring_drops))});
  }
  TextTable drops({"Drop reason", "Packets"});
  for (std::size_t r = 1; r < shm::kDropReasonCount; ++r) {
    const auto n = get(region_->dp.drops[r]);
    if (n != 0) {
      drops.add_row({shm::to_string(static_cast<shm::DropReason>(r)), u64(n)});
    }
  }
  return ports.render() + "\n" + (drops.empty() ? std::string("no drops\n") : drops.render());
}

std::string ControlPlane::show_mac(const std::vector<std::string>& t) const {
  std::optional<VlanId> vlan;
  std::optional<PortId> port;
  for (std::size_t i = 3; i + 1 < t.size(); i += 2) {
    if (t[i] == "vlan") {
      vlan = static_cast<VlanId>(std::stoul(t[i + 1]));
    } else if (t[i] == "interface") {
      port = config_.port_index(t[i + 1]);
      if (!port) {
        return "% no such interface '" + t[i + 1] + "'\n";
      }
    }
  }
  std::vector<const l2::FdbEntry*> rows;
  for (const auto& [key, e] : fdb_.entries()) {
    if ((!vlan || e.vlan == *vlan) && (!port || e.port == *port)) {
      rows.push_back(&e);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
    return a->vlan != b->vlan ? a->vlan < b->vlan : a->mac < b->mac;
  });
  const auto now = l2::Fdb::Clock::now();
  TextTable table({"VLAN", "MAC address", "Type", "Port", "Age"});
  for (const auto* e : rows) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - e->last_seen).count();
    table.add_row({std::to_string(e->vlan), e->mac.to_string(), e->is_static ? "static" : "dynamic",
                   config_.ports[e->port].name, e->is_static ? "-" : std::to_string(age) + "s"});
  }
  return table.render() + "total " + std::to_string(rows.size()) + " of " +
         std::to_string(fdb_.size()) + " entries; aging " +
         std::to_string(config_.mac_aging_seconds) + "s; moves " + std::to_string(fdb_.moves()) +
         "\n";
}

std::string ControlPlane::show_spanning_tree() const {
  if (!rstp_) {
    return "spanning tree is disabled (all ports forward)\n";
  }
  const auto bid = [](stp::BridgeId id) {
    std::ostringstream o;
    o << (id >> 48U) << "." << MacAddress::from_u64(id & 0xFFFFFFFFFFFFULL).to_string();
    return o.str();
  };
  const stp::Rstp& r = *rstp_;
  const std::optional<PortId> root_port = r.root_port();
  std::ostringstream out;
  out << "Bridge ID:   " << bid(r.bridge_id()) << "\n"
      << "Root ID:     " << bid(r.root_id())
      << (r.is_root_bridge() ? "  (this bridge is the root)" : "") << "\n"
      << "Root cost:   " << r.root_cost() << "\n"
      << "Root port:   " << (root_port ? config_.ports[*root_port].name : std::string("-")) << "\n"
      << "Timers:      hello " << r.config().hello_time.count() << "s, max age "
      << r.config().max_age.count() << "s, forward delay " << r.config().forward_delay.count()
      << "s\n"
      << "Topology changes: " << r.topology_changes();
  if (const auto last = r.last_topology_change()) {
    out << ", last "
        << std::chrono::duration_cast<std::chrono::seconds>(stp::Clock::now() - *last).count()
        << "s ago";
  }
  out << "\n\n";
  TextTable table(
      {"Port", "Role", "State", "Cost", "Prio.Nbr", "Edge", "Designated bridge", "BPDU tx/rx"});
  for (std::size_t i = 0; i < config_.ports.size(); ++i) {
    const auto& st = r.status(static_cast<PortId>(i));
    const auto& pc = r.port_config(static_cast<PortId>(i));
    std::ostringstream prio;
    prio << unsigned{pc.priority} << "." << (st.port_id & 0x0FFFU);
    table.add_row({config_.ports[i].name, st.enabled ? stp::to_string(st.role) : "disabled",
                   st.enabled ? stp::to_string(st.state) : "-", std::to_string(pc.path_cost),
                   prio.str(), st.oper_edge ? "yes" : "no",
                   st.received ? bid(st.received->bridge) : std::string("-"),
                   std::to_string(st.bpdus_sent) + "/" + std::to_string(st.bpdus_received)});
  }
  return out.str() + table.render();
}

std::string ControlPlane::show_igmp() const {
  const auto port_list = [this](const std::map<PortId, igmp::Clock::time_point>& ports) {
    std::string out;
    for (const auto& [port, expiry] : ports) {
      out += (out.empty() ? "" : ",") + config_.ports[port].name;
    }
    return out;
  };
  const auto now = igmp::Clock::now();
  std::ostringstream out;
  out << "IGMP snooping: " << (config_.igmp_snooping ? "enabled" : "disabled") << "; querier: "
      << (config_.igmp_querier ? "on (every " + std::to_string(config_.igmp_query_interval) + "s)"
                               : std::string("off"))
      << "; fast-leave on host ports: " << (config_.igmp_fast_leave ? "on" : "off") << "\n\n";
  TextTable routers({"VLAN", "Snooping", "Mrouter ports"});
  for (const auto& [vid, vlan] : config_.vlans) {
    (void)vlan;
    const auto it = snooper_.mrouter_ports().find(vid);
    routers.add_row(
        {std::to_string(vid), config_.snooping_in(vid) ? "on" : "off",
         it == snooper_.mrouter_ports().end() ? std::string("-") : port_list(it->second)});
  }
  TextTable groups({"VLAN", "Group", "Ports", "Expires"});
  for (const auto& [key, ports] : snooper_.groups()) {
    auto soonest = ports.begin()->second;
    for (const auto& [port, expiry] : ports) {
      soonest = std::min(soonest, expiry);
    }
    groups.add_row(
        {std::to_string(key.vlan), key.group.to_string(), port_list(ports),
         std::to_string(std::chrono::duration_cast<std::chrono::seconds>(soonest - now).count()) +
             "s"});
  }
  return out.str() + routers.render() + "\n" +
         (groups.empty() ? std::string("no groups\n") : groups.render());
}

std::string ControlPlane::show_ip_interface() const {
  TextTable table({"Interface", "Address", "Status", "MAC"});
  for (const auto& [vid, prefix] : config_.svis) {
    const bool up = router_.svi_in_vlan(vid) != nullptr;
    table.add_row({"vlan" + std::to_string(vid), prefix.to_string(),
                   up ? "up" : "down (no such VLAN)", bridge_mac_.to_string()});
  }
  return table.empty() ? std::string("no IP interfaces\n") : table.render();
}

std::string ControlPlane::show_ip_route() const {
  TextTable table({"Prefix", "Type", "Next hop", "Interface", "Status"});
  for (const auto& r : router_.routes()) {
    table.add_row({r.prefix.to_string(), r.connected ? "connected" : "static",
                   r.connected ? std::string("-") : r.nexthop.to_string(),
                   r.vlan != 0 ? "vlan" + std::to_string(r.vlan) : std::string("-"),
                   r.active ? "active" : "inactive (next hop unreachable)"});
  }
  const std::string routing =
      config_.ip_routing ? "IP routing is enabled\n" : "IP routing is disabled\n";
  return routing + (table.empty() ? std::string("no routes\n") : table.render());
}

std::string ControlPlane::show_arp() const {
  const auto now = l3::Clock::now();
  TextTable table({"Address", "MAC", "Interface", "Age"});
  for (const auto& [addr, n] : router_.neighbors()) {
    table.add_row(
        {addr.to_string(), n.mac.to_string(), "vlan" + std::to_string(n.vlan),
         std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - n.updated).count()) +
             "s"});
  }
  return (table.empty() ? std::string("no ARP entries\n") : table.render()) +
         "resolving: " + std::to_string(router_.pending()) + "\n";
}

std::string ControlPlane::show_dataplane() const {
  const auto& dp = region_->dp;
  const std::uint64_t now = sys::monotonic_ns();
  const std::uint64_t beat = dp.heartbeat_ns.load();
  std::ostringstream out;
  out << "attached:        " << (dataplane_attached() ? "yes" : "no") << " (pid " << dp.pid.load()
      << ")\n"
      << "heartbeat age:   "
      << (beat == 0 ? std::string("never") : u64((now - beat) / 1000) + " us") << "\n"
      << "batches:         " << get(dp.batches) << "\n"
      << "rx/tx packets:   " << get(dp.rx_packets) << " / " << get(dp.tx_packets) << "\n"
      << "learn events:    " << get(dp.learn_events) << " sent, " << learn_events_ << " processed\n"
      << "max batch time:  " << get(dp.max_batch_ns) / 1000 << " us\n"
      << "table publishes: " << publishes_ << " (seq " << publisher_->sequence() << ", acked "
      << dp.ack_seq.load() << ")\n"
      << "max grace wait:  "
      << std::chrono::duration_cast<std::chrono::microseconds>(publisher_->max_wait()).count()
      << " us, timeouts " << publisher_->timeouts() << "\n"
      << "publish mode:    " << shm::to_string(region_->publish_mode) << "\n";
  if (region_->publish_mode != shm::PublishMode::kQsbr) {
    const std::uint64_t waits = get(dp.lock_waits);
    out << "table lock:      " << waits << " batches, mean wait "
        << (waits == 0 ? 0 : get(dp.lock_wait_total_ns) / waits) << " ns, max wait "
        << get(dp.max_lock_wait_ns) / 1000 << " us\n";
  }
  TextTable rings({"Ring", "Depth", "Capacity", "Dropped"});
  rings.add_row({"learn (dp->cp)", u64(region_->learn.size()), u64(region_->learn.kCapacity),
                 u64(region_->learn.drops())});
  rings.add_row({"punt (dp->cp)", u64(region_->punt.size()), u64(region_->punt.kCapacity),
                 u64(region_->punt.drops())});
  rings.add_row({"inject (cp->dp)", u64(region_->inject.size()), u64(region_->inject.kCapacity),
                 u64(region_->inject.drops())});
  rings.add_row({"trace (dp->cp)", u64(region_->trace.size()), u64(region_->trace.kCapacity),
                 u64(region_->trace.drops())});
  return out.str() + "\n" + rings.render();
}

}  // namespace softswitch::cp
