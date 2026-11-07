#pragma once

/// @file
/// softswitchd: owns configuration and protocol state, publishes tables to the data plane.

#include <memory>
#include <string>
#include <vector>

#include "softswitch/common/types.hpp"
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

  /// Creates the region, publishes initial tables and starts listening for the data plane.
  void start();

  [[nodiscard]] std::string dataplane_socket_path() const;
  [[nodiscard]] const MacAddress& bridge_mac() const noexcept { return bridge_mac_; }
  [[nodiscard]] bool dataplane_attached() const noexcept { return dp_conn_.valid(); }

  /// Requests a table publish at the end of the current event-loop iteration.
  void schedule_publish();

 private:
  struct Port {
    std::string name;
    int ifindex = 0;
    bool admin_up = true;
    bool oper_up = true;
    VlanId pvid = 1;
  };

  void accept_dataplane();
  void detach_dataplane(const char* why);
  void on_notify();
  void publish_now();
  void build_tables(shm::Tables& tables) const;

  EventLoop* loop_;
  ControlPlaneOptions options_;
  MacAddress bridge_mac_;
  std::vector<Port> ports_;

  sys::SharedMemory memory_;
  shm::SharedRegion* region_ = nullptr;
  std::unique_ptr<shm::TablePublisher> publisher_;
  sys::UniqueFd notify_fd_;
  sys::UniqueFd kick_fd_;
  sys::UniqueFd listener_;
  sys::UniqueFd dp_conn_;
  bool publish_scheduled_ = false;
  bool publish_pending_ = false;
};

}  // namespace softswitch::cp
