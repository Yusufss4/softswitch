#pragma once

/// @file
/// softswitchd: owns configuration and protocol state, publishes tables to the data plane.

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/cp/cli_server.hpp"
#include "softswitch/cp/config.hpp"
#include "softswitch/cp/event_loop.hpp"
#include "softswitch/cp/netlink.hpp"
#include "softswitch/gui/http.hpp"
#include "softswitch/igmp/snooper.hpp"
#include "softswitch/l2/fdb.hpp"
#include "softswitch/l3/router.hpp"
#include "softswitch/shm/publisher.hpp"
#include "softswitch/shm/region.hpp"
#include "softswitch/stp/rstp.hpp"
#include "softswitch/sys/fd.hpp"
#include "softswitch/sys/shared_memory.hpp"

namespace softswitch::cp {

struct ControlPlaneOptions {
  std::string name = "sw1";
  std::vector<std::string> ports;
  std::string run_dir = "/tmp/softswitch";
  std::string config_file;      ///< Startup config (CLI commands, one per line); optional.
  std::uint16_t http_port = 0;  ///< Read-only web GUI; 0 = off.
  std::string http_bind = "0.0.0.0";
};

/// Derives a stable, locally administered MAC from the switch name (bridge ID / router MAC).
[[nodiscard]] MacAddress derive_bridge_mac(const std::string& name) noexcept;

class ControlPlane {
 public:
  ControlPlane(EventLoop& loop, ControlPlaneOptions options);
  ~ControlPlane();
  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;
  ControlPlane(ControlPlane&&) = delete;
  ControlPlane& operator=(ControlPlane&&) = delete;

  /// Loads the startup config, creates the region, publishes the initial tables and starts the
  /// data-plane and CLI sockets.
  void start();

  /// Runs one CLI command and returns its output (errors start with "% ").
  [[nodiscard]] std::string execute(const std::string& line);

  [[nodiscard]] std::string dataplane_socket_path() const;
  [[nodiscard]] std::string cli_socket_path() const;
  [[nodiscard]] const MacAddress& bridge_mac() const noexcept { return bridge_mac_; }
  [[nodiscard]] bool dataplane_attached() const noexcept { return dp_conn_.valid(); }
  [[nodiscard]] const SwitchConfig& config() const noexcept { return config_; }

  /// Requests a table publish at the end of the current event-loop iteration.
  void schedule_publish();

 private:
  struct PortRuntime {
    int ifindex = 0;
    bool oper_up = true;  ///< Until the first netlink dump says otherwise.
    MacAddress mac;
    std::uint64_t link_changes = 0;
  };

  void load_startup_config(const std::string& path);
  void accept_dataplane();
  void detach_dataplane(const char* why);
  void on_notify();
  void on_link(const LinkEvent& event);
  /// (Re)creates the spanning-tree instance from the config, or tears it down when disabled.
  void restart_stp();
  /// Pushes port enablement, port parameters and bridge priority into RSTP.
  void sync_stp();
  /// Queues an untagged frame for transmission on exactly @p port (bypassing VLAN and STP).
  void inject_raw(PortId port, std::span<const std::uint8_t> frame);
  /// Queues an untagged frame to be forwarded within @p vlan as if it came from the CPU port.
  void inject_vlan(VlanId vlan, std::span<const std::uint8_t> frame);
  void inject(const shm::PacketMessage& prototype, std::span<const std::uint8_t> frame);
  void on_igmp(const shm::PacketMessage& msg);
  /// Pushes SVIs and router settings from the config into the router.
  void sync_router();
  void send_router_output(const std::vector<l3::Output>& outputs);
  void igmp_tick();
  /// Sends an IGMP query in @p vlan (general when @p group is 0.0.0.0).
  void send_igmp_query(VlanId vlan, Ipv4Address group, std::uint8_t max_resp_tenths);
  void publish_now();
  void build_tables(shm::Tables& tables) const;
  /// Re-applies static MACs from the config and drops dynamic entries the config invalidated.
  void reconcile_fdb();
  /// Folds the data plane's hit bitmap into the FDB and ages out idle entries.
  void age_fdb();
  [[nodiscard]] std::string clear(const std::vector<std::string>& tokens);

  // show commands (show.cpp)
  [[nodiscard]] std::string show(const std::vector<std::string>& tokens) const;
  [[nodiscard]] std::string show_version() const;
  [[nodiscard]] std::string show_interfaces() const;
  [[nodiscard]] std::string show_vlan() const;
  [[nodiscard]] std::string show_counters() const;
  [[nodiscard]] std::string show_dataplane() const;
  [[nodiscard]] std::string show_mac(const std::vector<std::string>& tokens) const;
  [[nodiscard]] std::string show_spanning_tree() const;
  [[nodiscard]] std::string show_igmp() const;
  [[nodiscard]] std::string show_ip_interface() const;
  [[nodiscard]] std::string show_arp() const;
  [[nodiscard]] std::string show_ip_route() const;

  // web GUI (gui_api.cpp)
  void start_gui();
  void drain_traces();
  [[nodiscard]] std::string state_json() const;
  [[nodiscard]] std::string traces_json() const;
  [[nodiscard]] std::string trace_json(const shm::TraceRecord& record) const;

  EventLoop* loop_;
  ControlPlaneOptions options_;
  MacAddress bridge_mac_;
  SwitchConfig config_;
  std::vector<PortRuntime> runtime_;
  l2::Fdb fdb_{shm::kFdbSlots / 2};
  std::unique_ptr<stp::Rstp> rstp_;
  std::vector<shm::StpState> stp_state_;
  std::uint64_t inject_drops_ = 0;
  igmp::Snooper snooper_;
  l3::Router router_{l3::RouterConfig{}};
  std::unique_ptr<gui::HttpServer> http_;
  std::deque<shm::TraceRecord> recent_traces_;  ///< Last kRecentTraces sampled decisions.
  std::uint64_t traces_seen_ = 0;
  static constexpr std::size_t kRecentTraces = 1000;
  std::map<VlanId, stp::Clock::time_point> next_query_;  ///< Querier schedule per VLAN.
  std::map<VlanId, int> startup_queries_left_;
  std::uint64_t learn_events_ = 0;

  sys::SharedMemory memory_;
  shm::SharedRegion* region_ = nullptr;
  std::unique_ptr<shm::TablePublisher> publisher_;
  sys::UniqueFd notify_fd_;
  sys::UniqueFd kick_fd_;
  sys::UniqueFd listener_;
  sys::UniqueFd dp_conn_;
  std::unique_ptr<CliServer> cli_;
  std::unique_ptr<LinkMonitor> links_;
  std::uint64_t publishes_ = 0;
  bool publish_scheduled_ = false;
  bool publish_pending_ = false;
};

}  // namespace softswitch::cp
