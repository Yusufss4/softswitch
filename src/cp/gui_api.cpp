// The read-only web GUI: JSON state, the recent trace buffer and the live trace stream.
#include <chrono>

#include "softswitch/common/log.hpp"
#include "softswitch/cp/control_plane.hpp"
#include "softswitch/gui/assets.hpp"
#include "softswitch/gui/json.hpp"
#include "softswitch/sys/realtime.hpp"
#include "softswitch/version.hpp"

namespace softswitch::cp {
namespace {

std::uint64_t get(const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); }

std::string bridge_id_text(std::uint64_t id) {
  return std::to_string(id >> 48U) + "." + MacAddress::from_u64(id & 0xFFFFFFFFFFFFULL).to_string();
}

}  // namespace

void ControlPlane::start_gui() {
  http_ = std::make_unique<gui::HttpServer>(*loop_, options_.http_bind, options_.http_port);
  http_->route("/api/state", [this](const gui::HttpRequest&) {
    return gui::HttpResponse{200, "application/json", state_json()};
  });
  http_->route("/api/traces", [this](const gui::HttpRequest&) {
    return gui::HttpResponse{200, "application/json", traces_json()};
  });
  http_->events_at("/events");
  http_->fallback([](const gui::HttpRequest& req) {
    if (const gui::Asset* a = gui::find_asset(req.path)) {
      return gui::HttpResponse{
          200, std::string(a->content_type),
          std::string(reinterpret_cast<const char*>(a->data.data()), a->data.size())};
    }
    return gui::HttpResponse{404, "text/plain", "not found\n"};
  });
  loop_->every(std::chrono::milliseconds{100}, [this] { drain_traces(); });
  log::info("web GUI on http://", options_.http_bind, ":", http_->port(), "/ (read-only)");
}

void ControlPlane::drain_traces() {
  shm::TraceRecord rec;
  std::size_t sent = 0;
  while (region_->trace.try_pop(rec)) {
    ++traces_seen_;
    recent_traces_.push_back(rec);
    if (recent_traces_.size() > kRecentTraces) {
      recent_traces_.pop_front();
    }
    // At most 200 live events per 100 ms per switch: browsers stay responsive under load, and
    // the full sample is still in /api/traces.
    if (http_ && http_->subscribers() != 0 && sent++ < 200) {
      http_->broadcast("trace", trace_json(rec));
    }
  }
}

std::string ControlPlane::trace_json(const shm::TraceRecord& r) const {
  const auto port_name = [this](PortId p) {
    return p < config_.ports.size() ? config_.ports[p].name : std::string("cpu");
  };
  gui::JsonWriter w;
  w.begin_object();
  w.field("sw", config_.hostname);
  w.field("t", r.timestamp_ns);
  w.field("in", port_name(r.in_port));
  w.field("vlan", unsigned{r.vlan});
  w.field("out_vlan", unsigned{r.out_vlan});
  w.field("decision", shm::to_string(r.decision));
  w.field("reason", shm::to_string(r.reason));
  w.key("out").begin_array();
  for (PortId p = 0; p < kMaxPorts; ++p) {
    if (has_port(r.out_ports, p)) {
      w.value(port_name(p));
    }
  }
  w.end_array();
  w.field("src", r.src.to_string());
  w.field("dst", r.dst.to_string());
  w.field("ethertype", unsigned{r.ethertype});
  w.field("proto", unsigned{r.ip_proto});
  if (r.ethertype == 0x0800) {
    w.field("sip", r.src_ip.to_string());
    w.field("dip", r.dst_ip.to_string());
  }
  w.field("len", unsigned{r.len});
  w.end_object();
  return w.take();
}

std::string ControlPlane::traces_json() const {
  std::string out = "[";
  bool first = true;
  for (const auto& r : recent_traces_) {
    out += first ? "" : ",";
    out += trace_json(r);
    first = false;
  }
  return out + "]";
}

std::string ControlPlane::state_json() const {
  const shm::Tables& live = publisher_->live();
  const auto& dp = region_->dp;
  const auto now = std::chrono::steady_clock::now();
  gui::JsonWriter w;
  w.begin_object();
  w.field("name", config_.hostname);
  w.field("version", kVersion);
  w.field("bridge_mac", bridge_mac_.to_string());
  w.field("dataplane_attached", dataplane_attached());
  w.field("now_ns", sys::monotonic_ns());
  w.field("trace_sample", unsigned{config_.trace_sample});
  w.field("traces_seen", traces_seen_);
  w.field("sse_dropped", http_ ? http_->dropped_events() : std::uint64_t{0});

  // Spanning tree summary.
  w.key("stp").begin_object();
  w.field("enabled", static_cast<bool>(rstp_));
  if (rstp_) {
    w.field("bridge_id", bridge_id_text(rstp_->bridge_id()));
    w.field("root_id", bridge_id_text(rstp_->root_id()));
    w.field("is_root", rstp_->is_root_bridge());
    w.field("root_cost", unsigned{rstp_->root_cost()});
    w.field("topology_changes", rstp_->topology_changes());
  }
  w.end_object();

  // Ports with counters and STP role/state.
  w.key("ports").begin_array();
  for (std::size_t i = 0; i < config_.ports.size(); ++i) {
    const auto& p = config_.ports[i];
    const auto& e = live.ports[i];
    const auto& c = dp.ports[i];
    w.begin_object();
    w.field("name", p.name);
    w.field("description", p.description);
    w.field("admin_up", !p.shutdown);
    w.field("oper_up", runtime_[i].oper_up);
    w.field("mode", p.mode == shm::PortMode::kTrunk ? "trunk" : "access");
    w.field("pvid", unsigned{e.pvid});
    w.field("stp_state", shm::to_string(e.stp_state));
    w.field("stp_role",
            rstp_ ? stp::to_string(rstp_->status(static_cast<PortId>(i)).role) : "none");
    w.field("edge", p.stp_edge);
    w.field("rx_packets", get(c.rx_packets));
    w.field("tx_packets", get(c.tx_packets));
    w.field("rx_bytes", get(c.rx_bytes));
    w.field("tx_bytes", get(c.tx_bytes));
    w.field("drops", get(c.drops));
    w.end_object();
  }
  w.end_array();

  w.key("vlans").begin_array();
  for (const auto& [vid, vlan] : config_.vlans) {
    w.begin_object().field("id", unsigned{vid}).field("name", vlan.name);
    w.field("members", unsigned{live.vlans[vid].members})
        .field("untagged", unsigned{live.vlans[vid].untagged});
    w.field("snooping", config_.snooping_in(vid));
    if (const auto it = config_.svis.find(vid); it != config_.svis.end()) {
      w.field("svi", it->second.to_string());
    }
    w.end_object();
  }
  w.end_array();

  w.key("fdb").begin_array();
  std::size_t n = 0;
  for (const auto& [key, e] : fdb_.entries()) {
    if (++n > 500) {
      break;  // the GUI is a viewer, not a dump tool
    }
    w.begin_object().field("mac", e.mac.to_string()).field("vlan", unsigned{e.vlan});
    w.field("port", config_.ports[e.port].name).field("static", e.is_static);
    w.field("age",
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - e.last_seen).count()));
    w.end_object();
  }
  w.end_array();

  w.key("mdb").begin_array();
  for (const auto& [gk, ports] : snooper_.groups()) {
    w.begin_object().field("vlan", unsigned{gk.vlan}).field("group", gk.group.to_string());
    w.key("ports").begin_array();
    for (const auto& [port, expiry] : ports) {
      w.value(config_.ports[port].name);
    }
    w.end_array().end_object();
  }
  w.end_array();

  w.key("arp").begin_array();
  for (const auto& [addr, nb] : router_.neighbors()) {
    w.begin_object()
        .field("ip", addr.to_string())
        .field("mac", nb.mac.to_string())
        .field("vlan", unsigned{nb.vlan});
    w.end_object();
  }
  w.end_array();

  w.key("routes").begin_array();
  for (const auto& r : router_.routes()) {
    w.begin_object().field("prefix", r.prefix.to_string()).field("connected", r.connected);
    w.field("vlan", unsigned{r.vlan})
        .field("nexthop", r.connected ? std::string("-") : r.nexthop.to_string());
    w.field("active", r.active).end_object();
  }
  w.end_array();

  w.key("drops").begin_object();
  for (std::size_t r = 1; r < shm::kDropReasonCount; ++r) {
    if (const auto v = get(dp.drops[r]); v != 0) {
      w.field(shm::to_string(static_cast<shm::DropReason>(r)), v);
    }
  }
  w.end_object();
  w.field("rx_packets", get(dp.rx_packets));
  w.field("tx_packets", get(dp.tx_packets));
  w.end_object();
  return w.take();
}

}  // namespace softswitch::cp
