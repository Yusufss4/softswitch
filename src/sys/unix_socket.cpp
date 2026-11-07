#include "softswitch/sys/unix_socket.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace softswitch::sys {
namespace {

sockaddr_un make_address(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    throw std::invalid_argument("unix socket path too long: " + path);
  }
  std::memcpy(static_cast<void*>(&addr.sun_path[0]), path.c_str(), path.size() + 1);
  return addr;
}

const sockaddr* as_sockaddr(const sockaddr_un& addr) noexcept {
  return reinterpret_cast<const sockaddr*>(&addr);
}

}  // namespace

UniqueFd unix_listen(const std::string& path, int type, int backlog) {
  UniqueFd fd{::socket(AF_UNIX, type | SOCK_CLOEXEC, 0)};
  if (!fd.valid()) {
    throw_errno("socket(AF_UNIX)");
  }
  const auto addr = make_address(path);
  ::unlink(path.c_str());
  if (::bind(fd.get(), as_sockaddr(addr), sizeof(addr)) < 0) {
    throw_errno("bind " + path);
  }
  if (::listen(fd.get(), backlog) < 0) {
    throw_errno("listen " + path);
  }
  return fd;
}

UniqueFd unix_connect(const std::string& path, int type) {
  UniqueFd fd{::socket(AF_UNIX, type | SOCK_CLOEXEC, 0)};
  if (!fd.valid()) {
    throw_errno("socket(AF_UNIX)");
  }
  const auto addr = make_address(path);
  if (::connect(fd.get(), as_sockaddr(addr), sizeof(addr)) < 0) {
    throw_errno("connect " + path);
  }
  return fd;
}

void send_with_fds(int sock, std::span<const std::byte> payload, std::span<const int> fds) {
  if (payload.empty() || fds.size() > kMaxPassedFds) {
    throw std::invalid_argument("send_with_fds: empty payload or too many fds");
  }
  // sendmsg takes a non-const iovec but never writes through it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  iovec iov{const_cast<std::byte*>(payload.data()), payload.size()};
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * kMaxPassedFds)> control{};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  if (!fds.empty()) {
    msg.msg_control = control.data();
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
  }
  if (::sendmsg(sock, &msg, MSG_NOSIGNAL) < 0) {
    throw_errno("sendmsg");
  }
}

std::size_t recv_with_fds(int sock, std::span<std::byte> payload, std::vector<UniqueFd>& fds) {
  iovec iov{payload.data(), payload.size()};
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * kMaxPassedFds)> control{};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control.data();
  msg.msg_controllen = control.size();
  const ssize_t n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0) {
    throw_errno("recvmsg");
  }
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
      continue;
    }
    const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (std::size_t i = 0; i < count; ++i) {
      int fd = -1;
      std::memcpy(&fd, CMSG_DATA(cmsg) + (i * sizeof(int)), sizeof(int));
      fds.emplace_back(fd);
    }
  }
  return static_cast<std::size_t>(n);
}

}  // namespace softswitch::sys
