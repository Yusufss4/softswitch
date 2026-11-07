#include "softswitch/cp/control_plane.hpp"

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "softswitch/common/log.hpp"
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
    : loop_(&loop), options_(std::move(options)), bridge_mac_(derive_bridge_mac(options_.name)) {
  if (options_.ports.empty() || options_.ports.size() > kMaxPorts) {
    throw std::invalid_argument("need between 1 and 32 ports");
  }
  for (const auto& name : options_.ports) {
    if (name.size() >= shm::kIfNameLen) {
      throw std::invalid_argument("interface name too long: " + name);
    }
    Port port;
    port.name = name;
    port.ifindex = static_cast<int>(::if_nametoindex(name.c_str()));
    if (port.ifindex == 0) {
      throw std::runtime_error("no such interface: " + name);
    }
    ports_.push_back(port);
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

void ControlPlane::start() {
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
  // Retry publishes that timed out waiting for a grace period.
  loop_->every(std::chrono::milliseconds{20}, [this] {
    if (publish_pending_) {
      publish_now();
    }
  });
  log::info("switch ", options_.name, " bridge MAC ", bridge_mac_.to_string(), ", ", ports_.size(),
            " ports; waiting for data plane on ", dataplane_socket_path());
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
  shm::LearnEvent ev;
  while (region_->learn.try_pop(ev)) {
    // M2: feed the FDB.
  }
  while (const shm::PacketMessage* msg = region_->punt.front()) {
    // M3+: BPDUs, IGMP, ARP, ICMP.
    (void)msg;
    region_->punt.pop();
  }
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
  publish_pending_ = false;
}

void ControlPlane::build_tables(shm::Tables& t) const {
  t.num_ports = static_cast<std::uint32_t>(ports_.size());
  t.router_mac = bridge_mac_;
  t.flags = 0;
  t.vlans.fill(shm::VlanEntry{});
  for (std::size_t i = 0; i < ports_.size(); ++i) {
    const Port& port = ports_[i];
    shm::PortEntry& e = t.ports[i];
    e = shm::PortEntry{};
    std::copy_n(port.name.begin(), std::min(port.name.size(), shm::kIfNameLen - 1), e.name.begin());
    e.ifindex = port.ifindex;
    e.flags = static_cast<std::uint8_t>((port.admin_up ? shm::port_flag::kAdminUp : 0) |
                                        (port.oper_up ? shm::port_flag::kOperUp : 0));
    e.pvid = port.pvid;
    e.stp_state = shm::StpState::kForwarding;
    // M1: every port is an untagged member of its PVID.
    auto& vlan = t.vlans[port.pvid];
    vlan.flags = shm::vlan_flag::kActive;
    vlan.members |= port_bit(static_cast<PortId>(i));
    vlan.untagged |= port_bit(static_cast<PortId>(i));
  }
  shm::clear_dynamic(t);
}

}  // namespace softswitch::cp
