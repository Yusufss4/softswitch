#pragma once

/// @file
/// softswitchd: owns configuration and protocol state, publishes tables to the data plane.

#include <memory>
#include <string>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/cp/cli_server.hpp"
#include "softswitch/cp/config.hpp"
#include "softswitch/cp/event_loop.hpp"
#include "softswitch/shm/publisher.hpp"
#include "softswitch/shm/region.hpp"
#include "softswitch/sys/fd.hpp"
#include "softswitch/sys/shared_memory.hpp"

namespace softswitch::cp {

struct ControlPlaneOptions {
  std::string name = "sw1";
  std::vector<std::string> ports;
  std::string run_dir = "/tmp/softswitch";
  std::string config_file;  ///< Startup config (CLI commands, one per line); optional.
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
    bool oper_up = true;
  };

  void load_startup_config(const std::string& path);
  void accept_dataplane();
  void detach_dataplane(const char* why);
  void on_notify();
  void publish_now();
  void build_tables(shm::Tables& tables) const;

  // show commands (show.cpp)
  [[nodiscard]] std::string show(const std::vector<std::string>& tokens) const;
  [[nodiscard]] std::string show_version() const;
  [[nodiscard]] std::string show_interfaces() const;
  [[nodiscard]] std::string show_vlan() const;
  [[nodiscard]] std::string show_counters() const;
  [[nodiscard]] std::string show_dataplane() const;

  EventLoop* loop_;
  ControlPlaneOptions options_;
  MacAddress bridge_mac_;
  SwitchConfig config_;
  std::vector<PortRuntime> runtime_;

  sys::SharedMemory memory_;
  shm::SharedRegion* region_ = nullptr;
  std::unique_ptr<shm::TablePublisher> publisher_;
  sys::UniqueFd notify_fd_;
  sys::UniqueFd kick_fd_;
  sys::UniqueFd listener_;
  sys::UniqueFd dp_conn_;
  std::unique_ptr<CliServer> cli_;
  std::uint64_t publishes_ = 0;
  bool publish_scheduled_ = false;
  bool publish_pending_ = false;
};

}  // namespace softswitch::cp
