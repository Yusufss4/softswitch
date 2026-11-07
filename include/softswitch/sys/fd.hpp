#pragma once

/// @file
/// RAII file descriptors, errno exceptions and eventfd helpers.

#include <cstdint>
#include <string_view>
#include <utility>

namespace softswitch::sys {

/// Throws std::system_error for the current errno, prefixed with @p what.
[[noreturn]] void throw_errno(std::string_view what);

/// Owning file descriptor; closes on destruction. Move-only.
class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  /// Gives up ownership and returns the descriptor.
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

/// Creates a non-blocking, close-on-exec eventfd.
[[nodiscard]] UniqueFd make_eventfd();

/// Adds 1 to the eventfd counter. Never blocks; safe to call from the real-time thread.
bool eventfd_signal(int fd) noexcept;

/// Reads and resets the counter. Returns 0 if it was already zero.
std::uint64_t eventfd_drain(int fd) noexcept;

/// Sets O_NONBLOCK on @p fd.
void set_nonblocking(int fd);

}  // namespace softswitch::sys
