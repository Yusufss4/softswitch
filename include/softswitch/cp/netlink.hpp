#pragma once

/// @file
/// rtnetlink link monitoring: tells the control plane when a port's carrier comes and goes.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/cp/event_loop.hpp"
#include "softswitch/sys/fd.hpp"

namespace softswitch::cp {

struct LinkEvent {
  int ifindex = 0;
  std::string name;
  bool admin_up = false;  ///< IFF_UP
  bool carrier = false;   ///< IFF_LOWER_UP
  bool deleted = false;   ///< RTM_DELLINK
  MacAddress mac;

  /// Usable for forwarding: administratively up and with carrier.
  [[nodiscard]] bool oper_up() const noexcept { return admin_up && carrier && !deleted; }
};

/// Parses every RTM_NEWLINK / RTM_DELLINK message in a netlink datagram. Malformed or truncated
/// messages are skipped; nothing past @p datagram is ever read.
[[nodiscard]] std::vector<LinkEvent> parse_link_messages(std::span<const std::uint8_t> datagram);

/// Subscribes to RTMGRP_LINK and requests an initial dump, all on the control-plane event loop.
class LinkMonitor {
 public:
  using Callback = std::function<void(const LinkEvent&)>;

  LinkMonitor(EventLoop& loop, Callback callback);
  ~LinkMonitor();
  LinkMonitor(const LinkMonitor&) = delete;
  LinkMonitor& operator=(const LinkMonitor&) = delete;
  LinkMonitor(LinkMonitor&&) = delete;
  LinkMonitor& operator=(LinkMonitor&&) = delete;

  /// Asks the kernel for the state of every link (answered through the callback).
  void request_dump();

 private:
  void on_readable();

  EventLoop* loop_;
  Callback callback_;
  sys::UniqueFd fd_;
  std::uint32_t seq_ = 0;
};

}  // namespace softswitch::cp
