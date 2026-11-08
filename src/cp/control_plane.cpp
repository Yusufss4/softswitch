#include "softswitch/cp/control_plane.hpp"

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "softswitch/common/log.hpp"
#include "softswitch/pkt/bpdu.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/igmp.hpp"
#include "softswitch/pkt/ipv4.hpp"
#include "softswitch/shm/handshake.hpp"
#include "softswitch/shm/region_mapping.hpp"
#include "softswitch/sys/unix_socket.hpp"

namespace softswitch::cp {

MacAddress derive_bridge_mac(const std::string& name) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a
  for (const char c : name) {
    h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
  }
  MacAddress mac = MacAddress::from_u64(mix64(h));
  mac.bytes[0] = 0x02;  // locally administered, unicast
  return mac;
}

ControlPlane::ControlPlane(EventLoop& loop, ControlPlaneOptions options)
    : loop_(&loop),
      options_(std::move(options)),
      bridge_mac_(derive_bridge_mac(options_.name)),
      config_(SwitchConfig::with_ports(options_.name, options_.ports)) {
  if (options_.ports.empty() || options_.ports.size() > kMaxPorts) {
    throw std::invalid_argument("need between 1 and 32 ports");
  }
  for (const auto& name : options_.ports) {
    if (name.size() >= shm::kIfNameLen) {
      throw std::invalid_argument("interface name too long: " + name);
    }
    PortRuntime rt;
    rt.ifindex = static_cast<int>(::if_nametoindex(name.c_str()));
    if (rt.ifindex == 0) {
      throw std::runtime_error("no such interface: " + name);
    }
    runtime_.push_back(rt);
  }
}

ControlPlane::~ControlPlane() {
  if (listener_.valid()) {
    ::unlink(dataplane_socket_path().c_str());
  }
}

std::string ControlPlane::dataplane_socket_path() const {
  return options_.run_dir + "/" + options_.name + ".dp.sock";
}

std::string ControlPlane::cli_socket_path() const {
  return options_.run_dir + "/" + options_.name + ".cli.sock";
}

void ControlPlane::load_startup_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read startup config " + path);
  }
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    const auto tokens = tokenize(line);
    if (tokens.empty()) {
      continue;
    }
    const auto result = apply_config(config_, tokens);
    if (!result.ok) {
      throw std::runtime_error(path + ":" + std::to_string(number) + ": " + result.message);
    }
  }
  log::info("loaded startup config ", path, " (", number, " lines)");
}

std::string ControlPlane::execute(const std::string& line) {
  const auto tokens = tokenize(line);
  if (tokens.empty()) {
    return {};
  }
  if (tokens[0] == "show") {
    return show(tokens);
  }
  if (tokens[0] == "clear") {
    return clear(tokens);
  }
  if (!is_config_command(tokens)) {
    return "% unknown command '" + tokens[0] + "' (try 'show help')\n";
  }
  const auto result = apply_config(config_, tokens);
  if (!result.ok) {
    return "% " + result.message + "\n";
  }
  if (result.changed) {
    log::info("config: ", line);
    reconcile_fdb();
    sync_router();
    for (const auto& [vlan, routers] : std::map(snooper_.mrouter_ports())) {
      if (!config_.snooping_in(vlan)) {
        (void)snooper_.clear_vlan(vlan);
      }
    }
    for (const auto& [key, ports] : std::map(snooper_.groups())) {
      if (!config_.snooping_in(key.vlan)) {
        (void)snooper_.clear_vlan(key.vlan);
      }
    }
    const bool timers_changed =
        rstp_ && (rstp_->config().hello_time.count() != config_.stp_hello ||
                  rstp_->config().max_age.count() != config_.stp_max_age ||
                  rstp_->config().forward_delay.count() != config_.stp_forward_delay);
    // Publish first: RSTP may answer by sending BPDUs on a port that only now came up, and the
    // data plane only transmits on ports it already sees as up.
    publish_now();
    if (config_.stp_enabled != static_cast<bool>(rstp_) || timers_changed) {
      restart_stp();
    } else {
      sync_stp();
    }
    schedule_publish();
  }
  return result.message.empty() ? std::string{} : result.message + "\n";
}

void ControlPlane::start() {
  if (!options_.config_file.empty()) {
    load_startup_config(options_.config_file);
  }
  reconcile_fdb();
  sync_router();
  ::mkdir(options_.run_dir.c_str(), 0755);
  memory_ = shm::create_region_memory("softswitch-" + options_.name);
  region_ = &shm::construct_region(memory_);
  publisher_ = std::make_unique<shm::TablePublisher>(*region_);
  notify_fd_ = sys::make_eventfd();
  kick_fd_ = sys::make_eventfd();
  publish_now();

  listener_ = sys::unix_listen(dataplane_socket_path(), SOCK_SEQPACKET);
  loop_->add(listener_.get(), EPOLLIN, [this](std::uint32_t) { accept_dataplane(); });
  loop_->add(notify_fd_.get(), EPOLLIN, [this](std::uint32_t) { on_notify(); });
  loop_->every(std::chrono::seconds{1}, [this] {
    age_fdb();
    igmp_tick();
    send_router_output(router_.tick(l3::Clock::now()));
    if (router_.take_neighbors_changed()) {
      schedule_publish();
    }
  });
  // Retry publishes that timed out waiting for a grace period.
  loop_->every(std::chrono::milliseconds{20}, [this] {
    if (publish_pending_) {
      publish_now();
    }
  });
  links_ = std::make_unique<LinkMonitor>(*loop_, [this](const LinkEvent& ev) { on_link(ev); });
  restart_stp();
  loop_->every(std::chrono::milliseconds{100}, [this] {
    if (rstp_) {
      rstp_->tick(stp::Clock::now());
    }
  });
  if (options_.http_port != 0) {
    start_gui();
  }
  cli_ = std::make_unique<CliServer>(*loop_, cli_socket_path(),
                                     [this](const std::string& line) { return execute(line); });
  log::info("switch ", options_.name, " bridge MAC ", bridge_mac_.to_string(), ", ",
            runtime_.size(), " ports; waiting for data plane on ", dataplane_socket_path());
}

void ControlPlane::accept_dataplane() {
  sys::UniqueFd conn{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
  if (!conn.valid()) {
    return;
  }
  shm::DataPlaneHello hello;
  std::vector<sys::UniqueFd> unused;
  const auto n =
      sys::recv_with_fds(conn.get(), std::as_writable_bytes(std::span{&hello, 1}), unused);
  if (n != sizeof(hello) || hello.magic != shm::kRegionMagic ||
      hello.version != shm::kRegionVersion) {
    log::warn("rejecting data plane with a mismatched handshake");
    return;
  }
  if (dp_conn_.valid()) {
    log::warn("rejecting second data plane (pid ", hello.pid, "): one is already attached");
    return;
  }
  const shm::ControlPlaneWelcome welcome;
  const std::array<int, shm::kWelcomeFdCount> fds{memory_.fd(), notify_fd_.get(), kick_fd_.get()};
  sys::send_with_fds(conn.get(), std::as_bytes(std::span{&welcome, 1}), fds);
  dp_conn_ = std::move(conn);
  loop_->add(dp_conn_.get(), EPOLLIN | EPOLLRDHUP, [this](std::uint32_t events) {
    if ((events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) != 0) {
      detach_dataplane("connection closed");
      return;
    }
    std::array<std::byte, 64> buf{};
    std::vector<sys::UniqueFd> ignored;
    if (sys::recv_with_fds(dp_conn_.get(), buf, ignored) == 0) {
      detach_dataplane("connection closed");
    }
  });
  log::info("data plane attached (pid ", hello.pid, ")");
}

void ControlPlane::detach_dataplane(const char* why) {
  log::warn("data plane detached: ", why);
  loop_->remove(dp_conn_.get());
  dp_conn_.reset();
  // A crashed data plane can't clear these itself; without this the publisher would wait.
  region_->dp.attached.store(0, std::memory_order_seq_cst);
  region_->dp.idle.store(1, std::memory_order_seq_cst);
}

void ControlPlane::on_notify() {
  (void)sys::eventfd_drain(notify_fd_.get());
  const auto now = l2::Fdb::Clock::now();
  bool changed = false;
  shm::LearnEvent ev;
  while (region_->learn.try_pop(ev)) {
    ++learn_events_;
    // The data plane only reports frames it accepted, but re-check against the config: the
    // event may predate a reconfiguration.
    if (!port_in_vlan(config_, ev.port, ev.vlan)) {
      continue;
    }
    const auto result = fdb_.learn(ev.mac, ev.vlan, ev.port, now);
    if (result == l2::LearnResult::kNew || result == l2::LearnResult::kMoved) {
      changed = true;
      if (result == l2::LearnResult::kMoved) {
        log::debug("MAC move: ", ev.mac.to_string(), " vlan ", ev.vlan, " -> ",
                   config_.ports[ev.port].name);
      }
    }
  }
  while (const shm::PacketMessage* msg = region_->punt.front()) {
    const auto frame = std::span<const std::uint8_t>(msg->data.data(), msg->len);
    if (msg->reason == shm::PuntReason::kBpdu && rstp_ && msg->port < runtime_.size()) {
      if (const auto bpdu = pkt::parse_bpdu_frame(frame)) {
        rstp_->receive(msg->port, *bpdu, stp::Clock::now());
      }
    } else if (msg->reason == shm::PuntReason::kIgmp && msg->port < runtime_.size()) {
      on_igmp(*msg);
    } else if (msg->reason == shm::PuntReason::kArp) {
      send_router_output(router_.on_arp(msg->vlan, frame, l3::Clock::now()));
    } else if (msg->reason == shm::PuntReason::kToRouter) {
      send_router_output(router_.on_to_router(msg->vlan, frame));
    } else if (msg->reason == shm::PuntReason::kNeighborMiss) {
      send_router_output(router_.on_neighbor_miss(msg->vlan, frame, l3::Clock::now()));
    } else if (msg->reason == shm::PuntReason::kTtlExpired) {
      send_router_output(router_.on_ttl_expired(msg->vlan, frame));
    }
    region_->punt.pop();
  }
  if (changed) {
    schedule_publish();
  }
}

void ControlPlane::on_link(const LinkEvent& ev) {
  for (std::size_t i = 0; i < runtime_.size(); ++i) {
    PortRuntime& rt = runtime_[i];
    if (rt.ifindex != ev.ifindex) {
      continue;
    }
    rt.mac = ev.mac;
    const bool up = ev.oper_up();
    if (up == rt.oper_up) {
      return;
    }
    rt.oper_up = up;
    ++rt.link_changes;
    const auto port = static_cast<PortId>(i);
    log::info("link ", config_.ports[i].name, " is ", up ? "up" : "down",
              ev.admin_up ? "" : " (admin down)",
              ev.carrier || !ev.admin_up ? "" : " (no carrier)");
    if (!up) {
      // Stations behind a dead link are unreachable through it: forget them now rather than
      // black-holing their traffic until they age out.
      (void)snooper_.on_port_down(port);
      const auto flushed = fdb_.flush(port, std::nullopt);
      if (flushed != 0) {
        log::info("flushed ", flushed, " MAC entries learned on ", config_.ports[i].name);
      }
    }
    publish_now();  // see execute(): the data plane must see the port up before BPDUs go out
    sync_stp();
    schedule_publish();
    return;
  }
}

void ControlPlane::restart_stp() {
  stp_state_.assign(runtime_.size(), shm::StpState::kForwarding);
  rstp_.reset();
  if (!config_.stp_enabled) {
    log::info("spanning tree disabled: every port forwards");
    schedule_publish();
    return;
  }
  stp_state_.assign(runtime_.size(), shm::StpState::kDiscarding);
  stp::BridgeConfig bridge;
  bridge.priority = config_.stp_priority;
  bridge.mac = bridge_mac_;
  bridge.hello_time = std::chrono::seconds{config_.stp_hello};
  bridge.max_age = std::chrono::seconds{config_.stp_max_age};
  bridge.forward_delay = std::chrono::seconds{config_.stp_forward_delay};
  stp::Rstp::Callbacks cb;
  cb.send_bpdu = [this](PortId port, const pkt::Bpdu& bpdu) {
    std::array<std::uint8_t, pkt::kMinFrameLen> frame{};
    const MacAddress& src = runtime_[port].mac.is_zero() ? bridge_mac_ : runtime_[port].mac;
    const auto len = pkt::build_bpdu_frame(frame, src, bpdu);
    inject_raw(port, std::span{frame}.first(len));
  };
  cb.state_changed = [this](PortId port, stp::State state) {
    stp_state_[port] = state == stp::State::kForwarding ? shm::StpState::kForwarding
                       : state == stp::State::kLearning ? shm::StpState::kLearning
                                                        : shm::StpState::kDiscarding;
    log::debug("stp: ", config_.ports[port].name, " -> ", stp::to_string(state));
    schedule_publish();
  };
  cb.flush = [this](PortId except) {
    std::size_t removed = 0;
    for (std::size_t q = 0; q < runtime_.size(); ++q) {
      if (static_cast<PortId>(q) != except) {
        removed += fdb_.flush(static_cast<PortId>(q), std::nullopt);
      }
    }
    if (removed != 0) {
      log::debug("stp: topology change flushed ", removed, " MAC entries");
      schedule_publish();
    }
  };
  rstp_ = std::make_unique<stp::Rstp>(bridge, runtime_.size(), cb);
  log::info("spanning tree enabled: bridge priority ", config_.stp_priority);
  sync_stp();
}

void ControlPlane::sync_stp() {
  if (!rstp_) {
    return;
  }
  const auto now = stp::Clock::now();
  if (rstp_->config().priority != config_.stp_priority) {
    rstp_->set_bridge_priority(config_.stp_priority, now);
  }
  for (std::size_t i = 0; i < runtime_.size(); ++i) {
    const auto port = static_cast<PortId>(i);
    const PortConfig& pc = config_.ports[i];
    const stp::PortConfig& cur = rstp_->port_config(port);
    if (cur.path_cost != pc.stp_cost || cur.priority != pc.stp_priority ||
        cur.edge != pc.stp_edge) {
      rstp_->set_port_config(port, stp::PortConfig{pc.stp_cost, pc.stp_priority, pc.stp_edge}, now);
    }
    rstp_->set_port_enabled(port, !pc.shutdown && runtime_[i].oper_up, now);
  }
}

void ControlPlane::inject(const shm::PacketMessage& prototype,
                          std::span<const std::uint8_t> frame) {
  shm::PacketMessage* msg = region_->inject.reserve();
  if (msg == nullptr || frame.size() > msg->data.size()) {
    ++inject_drops_;
    return;
  }
  msg->port = prototype.port;
  msg->vlan = prototype.vlan;
  msg->len = static_cast<std::uint16_t>(frame.size());
  msg->reason = shm::PuntReason::kNone;
  msg->flags = prototype.flags;
  std::copy(frame.begin(), frame.end(), msg->data.begin());
  region_->inject.commit();
  (void)sys::eventfd_signal(kick_fd_.get());
}

void ControlPlane::inject_raw(PortId port, std::span<const std::uint8_t> frame) {
  shm::PacketMessage proto;
  proto.port = port;
  proto.flags = shm::inject_flag::kRaw;
  inject(proto, frame);
}

void ControlPlane::inject_vlan(VlanId vlan, std::span<const std::uint8_t> frame) {
  shm::PacketMessage proto;
  proto.vlan = vlan;
  proto.flags = shm::inject_flag::kVlan;
  inject(proto, frame);
}

void ControlPlane::on_igmp(const shm::PacketMessage& msg) {
  if (!config_.snooping_in(msg.vlan)) {
    return;
  }
  const auto frame = std::span<const std::uint8_t>(msg.data.data(), msg.len);
  const auto eth = pkt::parse_ethernet(frame);
  if (!eth) {
    return;
  }
  const auto l3 = frame.subspan(eth->payload_offset);
  const auto ip = pkt::parse_ipv4(l3);
  if (!ip || ip->protocol != pkt::kIpProtoIgmp) {
    return;
  }
  const auto m = pkt::parse_igmp(l3.subspan(ip->header_len, ip->total_len - ip->header_len));
  if (!m) {
    return;
  }
  const auto now = stp::Clock::now();
  const PortId port = msg.port;
  const bool fast_leave = config_.igmp_fast_leave && config_.ports[port].stp_edge;
  bool changed = false;
  auto leave = [&](Ipv4Address group) {
    changed = snooper_.on_leave(msg.vlan, group, port, fast_leave, now) || changed;
    if (!fast_leave && config_.igmp_querier) {
      // Others may still listen behind this port: ask them (group-specific query, 1 s).
      send_igmp_query(msg.vlan, group, 10);
    }
  };
  switch (m->type) {
    case pkt::kIgmpQuery:
      changed = snooper_.on_query(msg.vlan, port, now);
      break;
    case pkt::kIgmpV1Report:
    case pkt::kIgmpV2Report:
      changed = snooper_.on_report(msg.vlan, m->group, port, now);
      break;
    case pkt::kIgmpV2Leave:
      leave(m->group);
      break;
    case pkt::kIgmpV3Report:
      for (std::size_t i = 0; i < m->num_records; ++i) {
        const auto& r = m->records[i];
        if (pkt::IgmpMessage::record_is_join(r)) {
          changed = snooper_.on_report(msg.vlan, r.group, port, now) || changed;
        } else if (r.type == pkt::kIgmpChangeToInclude || r.type == pkt::kIgmpModeIsInclude) {
          leave(r.group);
        }
      }
      break;
    default:
      break;
  }
  if (changed) {
    log::debug("igmp: vlan ", msg.vlan, " membership changed on ", config_.ports[port].name);
    schedule_publish();
  }
}

void ControlPlane::send_igmp_query(VlanId vlan, Ipv4Address group, std::uint8_t max_resp_tenths) {
  std::array<std::uint8_t, pkt::kMinFrameLen> frame{};
  const auto len =
      pkt::build_igmp_query_frame(frame, bridge_mac_, Ipv4Address{}, group, max_resp_tenths);
  inject_vlan(vlan, std::span{frame}.first(len));
}

void ControlPlane::igmp_tick() {
  const auto now = stp::Clock::now();
  if (snooper_.tick(now)) {
    schedule_publish();
  }
  if (!config_.igmp_querier) {
    next_query_.clear();
    startup_queries_left_.clear();
    return;
  }
  const auto interval = std::chrono::seconds{config_.igmp_query_interval};
  for (const auto& [vid, vlan] : config_.vlans) {
    (void)vlan;
    if (!config_.snooping_in(vid)) {
      continue;
    }
    auto [it, fresh] = next_query_.try_emplace(vid, now);
    if (fresh) {
      startup_queries_left_[vid] = 2;  // RFC 3376 startup query count
    }
    if (now < it->second) {
      continue;
    }
    send_igmp_query(vid, Ipv4Address{}, 100);  // general query, 10 s max response
    int& startup = startup_queries_left_[vid];
    it->second = now + (startup > 0 ? interval / 4 : interval);
    startup = startup > 0 ? startup - 1 : 0;
  }
}

void ControlPlane::sync_router() {
  l3::RouterConfig rc;
  rc.mac = bridge_mac_;
  rc.arp_timeout = std::chrono::seconds{config_.arp_timeout};
  for (const auto& r : config_.static_routes) {
    rc.static_routes.push_back(l3::StaticRoute{r.prefix, r.nexthop});
  }
  for (const auto& [vid, prefix] : config_.svis) {
    if (config_.vlan_exists(vid)) {  // an SVI is only up while its VLAN exists
      rc.svis.push_back(l3::Svi{vid, prefix});
    }
  }
  router_.set_config(std::move(rc));
}

void ControlPlane::send_router_output(const std::vector<l3::Output>& outputs) {
  for (const auto& o : outputs) {
    inject_vlan(o.vlan, o.frame);
  }
  if (router_.take_neighbors_changed()) {
    schedule_publish();
  }
}

void ControlPlane::reconcile_fdb() {
  fdb_.clear_static();
  for (const auto& m : config_.static_macs) {
    if (const auto port = config_.port_index(m.port)) {
      (void)fdb_.add_static(m.mac, m.vlan, *port);
    }
  }
  const auto removed = fdb_.remove_if_not([this](const l2::FdbEntry& e) {
    return port_in_vlan(config_, e.port, e.vlan) && runtime_[e.port].oper_up;
  });
  if (removed != 0) {
    log::info("flushed ", removed, " MAC entries invalidated by configuration");
  }
}

void ControlPlane::age_fdb() {
  // Harvest and clear the hit bitmap atomically: a bit set after the exchange counts next time.
  std::vector<std::uint64_t> hits(region_->dp.fdb_hits.size());
  for (std::size_t i = 0; i < hits.size(); ++i) {
    hits[i] = region_->dp.fdb_hits[i].exchange(0, std::memory_order_relaxed);
  }
  const auto now = l2::Fdb::Clock::now();
  for (const auto& [key, entry] : fdb_.entries()) {
    const std::uint64_t bit = shm::fdb_hash(entry.mac, entry.vlan) & (shm::kHitBits - 1);
    if ((hits[bit / 64] & (std::uint64_t{1} << (bit % 64))) != 0) {
      fdb_.touch(entry.mac, entry.vlan, now);
    }
  }
  if (fdb_.age_out(now, std::chrono::seconds{config_.mac_aging_seconds}) != 0) {
    schedule_publish();
  }
}

std::string ControlPlane::clear(const std::vector<std::string>& t) {
  if (t.size() == 2 && t[1] == "arp") {
    const auto n = router_.neighbors().size();
    router_.clear_neighbors();
    schedule_publish();
    return "cleared " + std::to_string(n) + " ARP entries\n";
  }
  // clear mac address-table [dynamic] [vlan ID] [interface PORT]
  if (t.size() < 3 || t[1] != "mac" || t[2] != "address-table") {
    return "% usage: clear mac address-table [dynamic] [vlan ID] [interface PORT]\n";
  }
  std::optional<PortId> port;
  std::optional<VlanId> vlan;
  std::size_t i = 3;
  while (i < t.size()) {
    const std::string& word = t[i];
    if (word == "dynamic") {
      i += 1;
      continue;
    }
    if (i + 1 >= t.size()) {
      return "% '" + word + "' needs a value\n";
    }
    const std::string& arg = t[i + 1];
    i += 2;
    if (word == "vlan") {
      unsigned vid = 0;
      const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), vid);
      if (ec != std::errc{} || ptr != arg.data() + arg.size() || !valid_vlan(vid)) {
        return "% invalid VLAN id '" + arg + "'\n";
      }
      vlan = static_cast<VlanId>(vid);
    } else if (word == "interface") {
      port = config_.port_index(arg);
      if (!port) {
        return "% no such interface '" + arg + "'\n";
      }
    } else {
      return "% unexpected '" + word + "'\n";
    }
  }
  const auto removed = fdb_.flush(port, vlan);
  schedule_publish();
  return "cleared " + std::to_string(removed) + " entries\n";
}

void ControlPlane::schedule_publish() {
  if (publish_scheduled_) {
    return;
  }
  publish_scheduled_ = true;
  loop_->defer([this] {
    publish_scheduled_ = false;
    publish_now();
  });
}

void ControlPlane::publish_now() {
  shm::Tables* staged = publisher_->begin_update(std::chrono::milliseconds{5});
  if (staged == nullptr) {
    publish_pending_ = true;
    log::warn("table publish deferred: data plane has not finished its batch");
    return;
  }
  build_tables(*staged);
  publisher_->publish();
  ++publishes_;
  publish_pending_ = false;
}

void ControlPlane::build_tables(shm::Tables& t) const {
  t.router_mac = bridge_mac_;
  t.flags = rstp_ ? shm::global_flag::kStpEnabled : std::uint8_t{0};
  apply_l2_config(config_, t);
  for (std::size_t i = 0; i < runtime_.size() && i < kMaxPorts; ++i) {
    shm::PortEntry& e = t.ports[i];
    e.ifindex = runtime_[i].ifindex;
    e.flags = static_cast<std::uint8_t>(runtime_[i].oper_up ? (e.flags | shm::port_flag::kOperUp)
                                                            : (e.flags & ~shm::port_flag::kOperUp));
    e.stp_state = i < stp_state_.size() ? stp_state_[i] : shm::StpState::kForwarding;
  }
  shm::clear_dynamic(t);
  t.num_svis = 0;
  for (const auto& svi : router_.config().svis) {
    if (t.num_svis >= shm::kMaxSvis) {
      break;
    }
    t.svis[t.num_svis] = shm::SviEntry{svi.vlan, svi.address};
    t.vlans[svi.vlan].flags =
        static_cast<std::uint8_t>(t.vlans[svi.vlan].flags | shm::vlan_flag::kHasSvi);
    t.vlans[svi.vlan].svi_index = static_cast<std::uint8_t>(t.num_svis);
    ++t.num_svis;
  }
  for (const auto& [addr, n] : router_.neighbors()) {
    (void)shm::neighbor_insert(t, addr, n.mac, n.vlan);
  }
  if (config_.ip_routing && t.num_svis != 0) {
    t.flags = static_cast<std::uint8_t>(t.flags | shm::global_flag::kRoutingEnabled);
  }
  for (const auto& r : router_.routes()) {
    if (r.active) {
      (void)shm::route_insert(
          t, shm::RouteEntry{r.prefix, r.connected ? shm::route_flag::kConnected : std::uint8_t{0},
                             r.vlan, r.nexthop});
    }
  }
  for (const auto& [vid, vlan] : config_.vlans) {
    (void)vlan;
    if (config_.snooping_in(vid)) {
      t.vlans[vid].flags =
          static_cast<std::uint8_t>(t.vlans[vid].flags | shm::vlan_flag::kIgmpSnooping);
      t.vlans[vid].mrouters = snooper_.mrouters(vid) & t.vlans[vid].members;
    }
  }
  for (const auto& [key, ports] : snooper_.groups()) {
    if (config_.snooping_in(key.vlan) &&
        !shm::mdb_insert(t, key.group, key.vlan, snooper_.members(key.vlan, key.group))) {
      log::warn("data-plane MDB full; group ", key.group.to_string(), " not published");
      break;
    }
  }
  for (const auto& [key, e] : fdb_.entries()) {
    if (!shm::fdb_insert(t, e.mac, e.vlan, e.port, e.is_static)) {
      log::warn("data-plane FDB full; ", e.mac.to_string(), " not published");
      break;
    }
  }
}

}  // namespace softswitch::cp
