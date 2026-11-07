#include "softswitch/cp/cli_server.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>

#include "softswitch/common/log.hpp"
#include "softswitch/sys/unix_socket.hpp"

namespace softswitch::cp {
namespace {
constexpr std::size_t kMaxLine = std::size_t{64} * 1024;
}

CliServer::CliServer(EventLoop& loop, std::string path, Handler handler)
    : loop_(&loop),
      path_(std::move(path)),
      handler_(std::move(handler)),
      listener_(sys::unix_listen(path_, SOCK_STREAM)) {
  sys::set_nonblocking(listener_.get());
  loop_->add(listener_.get(), EPOLLIN, [this](std::uint32_t) { accept_client(); });
}

CliServer::~CliServer() {
  for (auto& [fd, session] : sessions_) {
    loop_->remove(fd);
  }
  loop_->remove(listener_.get());
  ::unlink(path_.c_str());
}

void CliServer::accept_client() {
  for (;;) {
    sys::UniqueFd fd{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
    if (!fd.valid()) {
      return;
    }
    const int raw = fd.get();
    auto session = std::make_unique<Session>();
    session->fd = std::move(fd);
    sessions_[raw] = std::move(session);
    loop_->add(raw, EPOLLIN | EPOLLRDHUP,
               [this, raw](std::uint32_t events) { on_event(raw, events); });
  }
}

void CliServer::on_event(int fd, std::uint32_t events) {
  const auto it = sessions_.find(fd);
  if (it == sessions_.end()) {
    return;
  }
  Session& s = *it->second;
  if ((events & EPOLLOUT) != 0 && !flush(s)) {
    return;
  }
  if ((events & EPOLLIN) != 0) {
    std::array<char, 4096> buf{};
    for (;;) {
      const ssize_t n = ::read(fd, buf.data(), buf.size());
      if (n > 0) {
        s.in.append(buf.data(), static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        events |= EPOLLRDHUP;
      }
      break;
    }
    std::size_t pos = 0;
    while ((pos = s.in.find('\n')) != std::string::npos) {
      std::string line = s.in.substr(0, pos);
      s.in.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      std::string reply;
      try {
        reply = handler_(line);
      } catch (const std::exception& e) {
        reply = std::string("% internal error: ") + e.what() + "\n";
      }
      s.out += reply;
      s.out.push_back('\0');
    }
    if (s.in.size() > kMaxLine) {
      log::warn("CLI client sent an over-long line; closing");
      close_session(fd);
      return;
    }
    if (!flush(s)) {
      return;
    }
  }
  if ((events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0 && s.out.empty()) {
    close_session(fd);
  }
}

bool CliServer::flush(Session& s) {
  while (!s.out.empty()) {
    const ssize_t n = ::send(s.fd.get(), s.out.data(), s.out.size(), MSG_NOSIGNAL);
    if (n > 0) {
      s.out.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      loop_->modify(s.fd.get(), EPOLLIN | EPOLLOUT | EPOLLRDHUP);
      return true;
    }
    close_session(s.fd.get());
    return false;
  }
  loop_->modify(s.fd.get(), EPOLLIN | EPOLLRDHUP);
  return true;
}

void CliServer::close_session(int fd) {
  loop_->remove(fd);
  sessions_.erase(fd);
}

}  // namespace softswitch::cp
