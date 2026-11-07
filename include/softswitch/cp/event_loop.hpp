#pragma once

/// @file
/// Single-threaded epoll event loop for the control plane.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "softswitch/sys/fd.hpp"

namespace softswitch::cp {

class EventLoop {
 public:
  using Handler = std::function<void(std::uint32_t events)>;

  EventLoop();
  ~EventLoop() = default;
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  /// Watches @p fd (not owned) for @p events (EPOLLIN, ...).
  void add(int fd, std::uint32_t events, Handler handler);
  void modify(int fd, std::uint32_t events);
  void remove(int fd);

  /// Calls @p callback every @p interval until the loop stops.
  void every(std::chrono::nanoseconds interval, std::function<void()> callback);
  /// Calls @p callback once at the end of the current iteration (coalesces work).
  void defer(std::function<void()> callback);
  /// Stops on SIGINT/SIGTERM (via signalfd).
  void stop_on_signals();

  void run();
  void stop() noexcept { running_ = false; }

 private:
  sys::UniqueFd epoll_;
  std::unordered_map<int, std::shared_ptr<Handler>> handlers_;
  std::vector<sys::UniqueFd> owned_;
  std::vector<std::function<void()>> deferred_;
  bool running_ = false;
};

}  // namespace softswitch::cp
