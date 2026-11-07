#include "softswitch/cp/netlink.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>

#include "softswitch/common/log.hpp"

namespace softswitch::cp {

LinkMonitor::LinkMonitor(EventLoop& loop, Callback callback)
    : loop_(&loop),
      callback_(std::move(callback)),
      fd_(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE)) {
  if (!fd_.valid()) {
    sys::throw_errno("socket(NETLINK_ROUTE)");
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = RTMGRP_LINK;
  if (::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    sys::throw_errno("bind(NETLINK_ROUTE, RTMGRP_LINK)");
  }
  loop_->add(fd_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
  request_dump();
}

LinkMonitor::~LinkMonitor() { loop_->remove(fd_.get()); }

void LinkMonitor::request_dump() {
  struct {
    nlmsghdr nh;
    ifinfomsg ifi;
  } req{};
  req.nh.nlmsg_len = sizeof(req);
  req.nh.nlmsg_type = RTM_GETLINK;
  req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.nh.nlmsg_seq = ++seq_;
  req.ifi.ifi_family = AF_UNSPEC;
  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  if (::sendto(fd_.get(), &req, sizeof(req), 0, reinterpret_cast<const sockaddr*>(&kernel),
               sizeof(kernel)) < 0) {
    log::warn("RTM_GETLINK dump request failed: errno ", errno);
  }
}

void LinkMonitor::on_readable() {
  std::array<std::uint8_t, 32768> buf{};
  for (;;) {
    const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
    if (n < 0) {
      if (errno == ENOBUFS) {
        // The kernel dropped notifications: our view may be stale, so ask again.
        log::warn("netlink overrun; re-dumping link state");
        request_dump();
        continue;
      }
      return;  // EAGAIN: drained
    }
    for (const auto& ev : parse_link_messages(std::span{buf}.first(static_cast<std::size_t>(n)))) {
      callback_(ev);
    }
  }
}

}  // namespace softswitch::cp
