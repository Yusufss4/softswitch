#include "softswitch/sys/fd.hpp"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace softswitch::sys {

void throw_errno(std::string_view what) {
  throw std::system_error(errno, std::generic_category(), std::string(what));
}

void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = fd;
}

UniqueFd make_eventfd() {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw_errno("eventfd");
  }
  return UniqueFd{fd};
}

bool eventfd_signal(int fd) noexcept {
  const std::uint64_t one = 1;
  return ::write(fd, &one, sizeof(one)) == static_cast<ssize_t>(sizeof(one));
}

std::uint64_t eventfd_drain(int fd) noexcept {
  std::uint64_t value = 0;
  if (::read(fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) {
    return 0;
  }
  return value;
}

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw_errno("fcntl(O_NONBLOCK)");
  }
}

}  // namespace softswitch::sys
