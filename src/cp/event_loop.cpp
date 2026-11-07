#include "softswitch/cp/event_loop.hpp"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>

namespace softswitch::cp {

EventLoop::EventLoop() : epoll_(::epoll_create1(EPOLL_CLOEXEC)) {
  if (!epoll_.valid()) {
    sys::throw_errno("epoll_create1");
  }
}

void EventLoop::add(int fd, std::uint32_t events, Handler handler) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
    sys::throw_errno("epoll_ctl(ADD)");
  }
  handlers_[fd] = std::make_shared<Handler>(std::move(handler));
}

void EventLoop::modify(int fd, std::uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
    sys::throw_errno("epoll_ctl(MOD)");
  }
}

void EventLoop::remove(int fd) {
  (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
  handlers_.erase(fd);
}

void EventLoop::every(std::chrono::nanoseconds interval, std::function<void()> callback) {
  sys::UniqueFd tfd{::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)};
  if (!tfd.valid()) {
    sys::throw_errno("timerfd_create");
  }
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
  itimerspec spec{};
  spec.it_interval.tv_sec = secs.count();
  spec.it_interval.tv_nsec = (interval - secs).count();
  spec.it_value = spec.it_interval;
  if (::timerfd_settime(tfd.get(), 0, &spec, nullptr) < 0) {
    sys::throw_errno("timerfd_settime");
  }
  const int fd = tfd.get();
  add(fd, EPOLLIN, [fd, cb = std::move(callback)](std::uint32_t) {
    std::uint64_t expirations = 0;
    if (::read(fd, &expirations, sizeof(expirations)) > 0) {
      cb();
    }
  });
  owned_.push_back(std::move(tfd));
}

void EventLoop::defer(std::function<void()> callback) { deferred_.push_back(std::move(callback)); }

void EventLoop::stop_on_signals() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
    sys::throw_errno("sigprocmask");
  }
  sys::UniqueFd sfd{::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)};
  if (!sfd.valid()) {
    sys::throw_errno("signalfd");
  }
  const int fd = sfd.get();
  add(fd, EPOLLIN, [this, fd](std::uint32_t) {
    signalfd_siginfo info{};
    if (::read(fd, &info, sizeof(info)) > 0) {
      stop();
    }
  });
  owned_.push_back(std::move(sfd));
}

void EventLoop::run() {
  running_ = true;
  std::array<epoll_event, 32> events{};
  // cppcheck-suppress knownConditionTrueFalse ; handlers clear running_ via stop()
  while (running_) {
    const int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      sys::throw_errno("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
      const auto& ev = events[static_cast<std::size_t>(i)];
      const auto it = handlers_.find(ev.data.fd);
      if (it == handlers_.end()) {
        continue;  // removed by an earlier handler in this batch
      }
      const std::shared_ptr<Handler> handler = it->second;  // survives self-removal
      (*handler)(ev.events);
    }
    while (!deferred_.empty()) {
      auto work = std::move(deferred_);
      deferred_.clear();
      for (const auto& fn : work) {
        fn();
      }
    }
  }
}

}  // namespace softswitch::cp
