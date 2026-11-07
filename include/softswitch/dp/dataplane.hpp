#pragma once

/// @file
/// The data-plane process: attaches to the control plane, opens the ports and runs the
/// forwarding thread.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "softswitch/dp/af_packet_port.hpp"
#include "softswitch/dp/pipeline.hpp"
#include "softswitch/shm/region.hpp"
#include "softswitch/sys/fd.hpp"
#include "softswitch/sys/shared_memory.hpp"

namespace softswitch::dp {

struct DataPlaneOptions {
  std::string control_socket;  ///< softswitchd's data-plane socket.
  int cpu = -1;                ///< Pin the forwarding thread to this CPU (-1 = don't).
  int priority = 50;           ///< SCHED_FIFO priority; 0 = keep SCHED_OTHER.
  bool require_rt = false;     ///< Fail instead of warning when RT setup is refused.
  TxMode tx_mode = TxMode::kRing;
  std::size_t batch = 64;  ///< Max frames per port per batch.
};

class DataPlane {
 public:
  explicit DataPlane(DataPlaneOptions options);
  ~DataPlane();
  DataPlane(const DataPlane&) = delete;
  DataPlane& operator=(const DataPlane&) = delete;
  DataPlane(DataPlane&&) = delete;
  DataPlane& operator=(DataPlane&&) = delete;

  /// Connects, attaches, opens ports and starts the forwarding thread. Throws on failure.
  void start();
  /// Blocks until the control plane goes away, SIGINT/SIGTERM arrives or stop() is called.
  void wait();
  void stop() noexcept;

 private:
  void forward_loop() noexcept;

  DataPlaneOptions options_;
  sys::UniqueFd control_;
  sys::SharedMemory memory_;
  shm::SharedRegion* region_ = nullptr;
  sys::UniqueFd notify_fd_;
  sys::UniqueFd kick_fd_;
  sys::UniqueFd stop_fd_;    ///< eventfd that wakes the forwarding thread and wait().
  sys::UniqueFd signal_fd_;  ///< SIGINT/SIGTERM, blocked in every thread and read in wait().
  std::vector<std::unique_ptr<PortIo>> ports_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::string rt_status_;
};

}  // namespace softswitch::dp
