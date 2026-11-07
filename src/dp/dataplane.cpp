#include "softswitch/dp/dataplane.hpp"

#include <poll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <stdexcept>

#include "softswitch/common/log.hpp"
#include "softswitch/shm/handshake.hpp"
#include "softswitch/shm/publisher.hpp"
#include "softswitch/shm/region_mapping.hpp"
#include "softswitch/sys/realtime.hpp"
#include "softswitch/sys/unix_socket.hpp"

namespace softswitch::dp {
namespace {

/// Egress that writes straight into the ports' TX rings and counts per port.
class PortEgress final : public Egress {
 public:
  PortEgress(std::vector<std::unique_ptr<PortIo>>& ports, shm::DataPlaneState& dp) noexcept
      : ports_(&ports), dp_(&dp) {}
  void emit(PortId port, std::span<const std::uint8_t> untagged,
            std::optional<std::uint16_t> tci) noexcept override {
    if (port >= ports_->size()) {
      return;
    }
    auto& counters = dp_->ports[port];
    if ((*ports_)[port]->tx(untagged, tci)) {
      shm::bump(counters.tx_packets);
      shm::bump(counters.tx_bytes, untagged.size() + (tci ? 4U : 0U));
      shm::bump(dp_->tx_packets);
    } else {
      shm::bump(counters.drops);
    }
  }

 private:
  std::vector<std::unique_ptr<PortIo>>* ports_;
  shm::DataPlaneState* dp_;
};

}  // namespace

DataPlane::DataPlane(DataPlaneOptions options) : options_(std::move(options)) {}

DataPlane::~DataPlane() {
  stop();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (region_ != nullptr) {
    region_->dp.attached.store(0, std::memory_order_seq_cst);
  }
}

void DataPlane::start() {
  // ---- handshake --------------------------------------------------------------------------
  control_ = sys::unix_connect(options_.control_socket, SOCK_SEQPACKET);
  shm::DataPlaneHello hello;
  hello.pid = static_cast<std::int32_t>(::getpid());
  sys::send_with_fds(control_.get(), std::as_bytes(std::span{&hello, 1}), {});

  shm::ControlPlaneWelcome welcome;
  std::vector<sys::UniqueFd> fds;
  const auto n =
      sys::recv_with_fds(control_.get(), std::as_writable_bytes(std::span{&welcome, 1}), fds);
  if (n != sizeof(welcome) || fds.size() != shm::kWelcomeFdCount) {
    throw std::runtime_error("control plane rejected the data plane (already attached?)");
  }
  if (welcome.magic != shm::kRegionMagic || welcome.version != shm::kRegionVersion ||
      welcome.region_size != sizeof(shm::SharedRegion)) {
    throw std::runtime_error("control plane speaks a different region version");
  }
  memory_ = sys::SharedMemory::attach(std::move(fds[shm::kWelcomeFdMemory]));
  region_ = &shm::attach_region(memory_);
  notify_fd_ = std::move(fds[shm::kWelcomeFdNotify]);
  kick_fd_ = std::move(fds[shm::kWelcomeFdKick]);
  stop_fd_ = sys::make_eventfd();

  // Block termination signals before spawning threads so the RT thread never runs a handler;
  // wait() reads them from a signalfd instead.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    throw std::runtime_error("pthread_sigmask failed");
  }
  signal_fd_.reset(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
  if (!signal_fd_.valid()) {
    sys::throw_errno("signalfd");
  }

  // ---- ports, from the tables the control plane already published --------------------------
  const shm::Tables& tables = region_->tables[region_->active.load() & 1U];
  for (std::uint32_t p = 0; p < tables.num_ports; ++p) {
    AfPacketOptions opts;
    opts.ifname = std::string(tables.ports[p].name.data());
    opts.tx_mode = options_.tx_mode;
    ports_.push_back(std::make_unique<AfPacketPort>(opts));
    log::info("port ", p, " = ", opts.ifname);
  }

  // ---- real-time setup ---------------------------------------------------------------------
  if (auto err = sys::lock_all_memory(); !err.empty()) {
    if (options_.require_rt) {
      throw std::runtime_error(err);
    }
    log::warn(err);
  }
  region_->dp.pid.store(hello.pid);
  region_->dp.attached.store(1, std::memory_order_seq_cst);

  thread_ = std::thread([this] {
    sys::name_current_thread("ss-forward");
    std::string status;
    if (options_.cpu >= 0) {
      if (auto err = sys::pin_current_thread(options_.cpu); !err.empty()) {
        status += err + "; ";
      }
    }
    if (options_.priority > 0) {
      if (auto err = sys::set_current_thread_fifo(options_.priority); !err.empty()) {
        status += err;
      }
    }
    if (!status.empty()) {
      log::warn("forwarding thread runs without full RT setup: ", status);
      if (options_.require_rt) {
        stop();
        return;
      }
    } else {
      log::info("forwarding thread: SCHED_FIFO ", options_.priority, ", cpu ", options_.cpu);
    }
    forward_loop();
  });
}

void DataPlane::stop() noexcept {
  if (!stopping_.exchange(true) && stop_fd_.valid()) {
    (void)sys::eventfd_signal(stop_fd_.get());
  }
}

void DataPlane::wait() {
  std::array<pollfd, 3> pfds{
      {{control_.get(), POLLIN, 0}, {stop_fd_.get(), POLLIN, 0}, {signal_fd_.get(), POLLIN, 0}}};
  while (!stopping_.load()) {
    if (::poll(pfds.data(), pfds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      std::array<std::byte, 16> buf{};
      std::vector<sys::UniqueFd> fds;
      if (sys::recv_with_fds(control_.get(), buf, fds) == 0) {
        log::info("control plane closed the connection; exiting");
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      break;
    }
    if ((pfds[2].revents & POLLIN) != 0) {
      log::info("signal received; exiting");
      break;
    }
  }
  stop();
}

void DataPlane::forward_loop() noexcept {
  shm::TableReader reader(*region_);
  Pipeline pipeline(*region_);
  PortEgress egress(ports_, region_->dp);
  std::vector<RxFrame> frames(options_.batch);

  std::vector<pollfd> pfds;
  pfds.reserve(ports_.size() + 2);
  for (const auto& port : ports_) {
    pfds.push_back({port->poll_fd(), POLLIN, 0});
  }
  pfds.push_back({kick_fd_.get(), POLLIN, 0});
  pfds.push_back({stop_fd_.get(), POLLIN, 0});

  auto& dp = region_->dp;
  while (!stopping_.load(std::memory_order_relaxed)) {
    const bool work =
        !region_->inject.empty() ||
        std::any_of(ports_.begin(), ports_.end(), [](const auto& p) { return p->rx_ready(); });
    if (!work) {
      reader.go_idle();
      (void)::poll(pfds.data(), pfds.size(), 1);
      if ((pfds[pfds.size() - 2].revents & POLLIN) != 0) {
        (void)sys::eventfd_drain(kick_fd_.get());
      }
      dp.heartbeat_ns.store(sys::monotonic_ns(), std::memory_order_relaxed);
      continue;
    }

    const std::uint64_t batch_start = sys::monotonic_ns();
    const shm::Tables& tables = reader.enter();
    pipeline.set_now(batch_start);
    std::uint64_t received = 0;
    for (std::size_t p = 0; p < ports_.size(); ++p) {
      const std::size_t n = ports_[p]->rx_burst(frames);
      for (std::size_t i = 0; i < n; ++i) {
        pipeline.process(tables, static_cast<PortId>(p), frames[i], egress);
      }
      ports_[p]->rx_release();
      received += n;
    }
    for (int i = 0; i < 64; ++i) {
      const shm::PacketMessage* msg = region_->inject.front();
      if (msg == nullptr) {
        break;
      }
      pipeline.process_inject(tables, *msg, egress);
      region_->inject.pop();
    }
    for (auto& port : ports_) {
      port->tx_flush();
    }
    reader.exit();

    if (pipeline.take_notify()) {
      (void)sys::eventfd_signal(notify_fd_.get());
    }
    shm::bump(dp.batches);
    shm::bump(dp.rx_packets, received);
    const std::uint64_t elapsed = sys::monotonic_ns() - batch_start;
    if (elapsed > dp.max_batch_ns.load(std::memory_order_relaxed)) {
      dp.max_batch_ns.store(elapsed, std::memory_order_relaxed);
    }
    dp.heartbeat_ns.store(batch_start, std::memory_order_relaxed);
  }
  reader.go_idle();
}

}  // namespace softswitch::dp
