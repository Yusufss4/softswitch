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

const char* stp_state_name(shm::StpState s) {
  switch (s) {
    case shm::StpState::kDisabled:
      return "disabled";
    case shm::StpState::kDiscarding:
      return "discarding";
    case shm::StpState::kLearning:
      return "learning";
    case shm::StpState::kForwarding:
      return "forwarding";
  }
  return "?";
}

const char* drop_reason_name(shm::DropReason r) {
  switch (r) {
    case shm::DropReason::kNone:
      return "none";
    case shm::DropReason::kPortDown:
      return "port-down";
    case shm::DropReason::kMalformed:
      return "malformed";
    case shm::DropReason::kUntaggedNotAllowed:
      return "untagged-not-allowed";
    case shm::DropReason::kTaggedOnAccess:
      return "tagged-on-access";
    case shm::DropReason::kVlanNotMember:
      return "vlan-not-member";
    case shm::DropReason::kStpBlocked:
      return "stp-blocked";
    case shm::DropReason::kReservedMulticast:
      return "reserved-multicast";
    case shm::DropReason::kSameSegment:
      return "same-segment";
    case shm::DropReason::kNoEgress:
      return "no-egress";
    case shm::DropReason::kNoRoute:
      return "no-route";
    case shm::DropReason::kPuntRingFull:
      return "punt-ring-full";
    case shm::DropReason::kCount:
      break;
  }
  return "?";
}

constexpr const char* kHelp =
    "show version | running-config | interfaces | vlan | counters | dataplane\n"
    "show mac address-table [vlan ID] [interface PORT]\n"
    "show spanning-tree\n"
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
                   e.pvid == 0 ? "-" : std::to_string(e.pvid), stp_state_name(e.stp_state),
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
  TextTable ports({"Port", "RX packets", "RX bytes", "TX packets", "TX bytes", "Drops"});
  for (std::size_t i = 0; i < config_.ports.size(); ++i) {
    const auto& c = region_->dp.ports[i];
    ports.add_row({config_.ports[i].name, u64(get(c.rx_packets)), u64(get(c.rx_bytes)),
                   u64(get(c.tx_packets)), u64(get(c.tx_bytes)), u64(get(c.drops))});
  }
  TextTable drops({"Drop reason", "Packets"});
  for (std::size_t r = 1; r < shm::kDropReasonCount; ++r) {
    const auto n = get(region_->dp.drops[r]);
    if (n != 0) {
      drops.add_row({drop_reason_name(static_cast<shm::DropReason>(r)), u64(n)});
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
      << " us, timeouts " << publisher_->timeouts() << "\n";
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
