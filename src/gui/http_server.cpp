#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <stdexcept>
#include <vector>

#include "softswitch/common/log.hpp"
#include "softswitch/gui/http.hpp"

namespace softswitch::gui {

HttpServer::HttpServer(cp::EventLoop& loop, const std::string& bind, std::uint16_t port)
    : loop_(&loop) {
  listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!listener_.valid()) {
    sys::throw_errno("socket(AF_INET)");
  }
  const int one = 1;
  (void)::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1) {
    throw std::invalid_argument("bad HTTP bind address: " + bind);
  }
  if (::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    sys::throw_errno("bind HTTP " + bind + ":" + std::to_string(port));
  }
  if (::listen(listener_.get(), 16) < 0) {
    sys::throw_errno("listen HTTP");
  }
  socklen_t len = sizeof(addr);
  (void)::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  loop_->add(listener_.get(), EPOLLIN, [this](std::uint32_t) { accept_clients(); });
}

HttpServer::~HttpServer() {
  for (const auto& [fd, c] : connections_) {
    loop_->remove(fd);
  }
  loop_->remove(listener_.get());
}

void HttpServer::route(const std::string& path, Handler handler) {
  routes_[path] = std::move(handler);
}

std::size_t HttpServer::subscribers() const noexcept {
  std::size_t n = 0;
  for (const auto& [fd, c] : connections_) {
    n += c->streaming ? 1U : 0U;
  }
  return n;
}

void HttpServer::accept_clients() {
  for (;;) {
    sys::UniqueFd fd{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
    if (!fd.valid()) {
      return;
    }
    const int raw = fd.get();
    auto conn = std::make_unique<Connection>();
    conn->fd = std::move(fd);
    connections_[raw] = std::move(conn);
    loop_->add(raw, EPOLLIN | EPOLLRDHUP,
               [this, raw](std::uint32_t events) { on_event(raw, events); });
  }
}

void HttpServer::on_event(int fd, std::uint32_t events) {
  const auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return;
  }
  Connection& c = *it->second;
  if ((events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0) {
    close(fd);
    return;
  }
  if ((events & EPOLLOUT) != 0 && !flush(c)) {
    return;
  }
  if ((events & EPOLLIN) == 0 || c.streaming || c.closing) {
    return;
  }
  std::array<char, 4096> buf{};
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      if (n == 0) {
        close(fd);
        return;
      }
      break;
    }
    c.in.append(buf.data(), static_cast<std::size_t>(n));
  }
  const auto end = c.in.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (c.in.size() > kMaxRequestHead) {
      c.out = render_response({413, "text/plain", "request too large\n"});
      c.closing = true;
      (void)flush(c);
    }
    return;
  }
  const auto request = parse_request_head(std::string_view(c.in).substr(0, end + 4));
  if (!request) {
    c.out = render_response({400, "text/plain", "bad request\n"});
    c.closing = true;
  } else {
    handle(c, *request);
  }
  (void)flush(c);
}

void HttpServer::handle(Connection& c, const HttpRequest& req) {
  if (req.method != "GET") {
    c.out = render_response({405, "text/plain", "read-only: GET only\n"});
    c.closing = true;
    return;
  }
  if (!events_path_.empty() && req.path == events_path_) {
    c.streaming = true;
    c.out =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\n";
    return;
  }
  HttpResponse response{404, "text/plain", "not found\n"};
  if (const auto it = routes_.find(req.path); it != routes_.end()) {
    response = it->second(req);
  } else if (fallback_) {
    response = fallback_(req);
  }
  c.out = render_response(response);
  c.closing = true;
}

void HttpServer::broadcast(std::string_view event, std::string_view data) {
  std::string message = "event: ";
  message.append(event).append("\ndata: ").append(data).append("\n\n");
  // flush() may close (and erase) a connection, so walk a snapshot of the descriptors.
  std::vector<int> fds;
  fds.reserve(connections_.size());
  for (const auto& [fd, c] : connections_) {
    if (c->streaming) {
      fds.push_back(fd);
    }
  }
  for (const int fd : fds) {
    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
      continue;
    }
    Connection& c = *it->second;
    if (c.out.size() + message.size() > kMaxSseBacklog) {
      ++dropped_events_;  // a slow browser never makes the control plane buffer without bound
      continue;
    }
    c.out += message;
    (void)flush(c);
  }
}

bool HttpServer::flush(Connection& c) {
  while (!c.out.empty()) {
    const ssize_t n = ::send(c.fd.get(), c.out.data(), c.out.size(), MSG_NOSIGNAL);
    if (n > 0) {
      c.out.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      loop_->modify(c.fd.get(), EPOLLIN | EPOLLOUT | EPOLLRDHUP);
      return true;
    }
    close(c.fd.get());
    return false;
  }
  if (c.closing) {
    close(c.fd.get());
    return false;
  }
  loop_->modify(c.fd.get(), EPOLLIN | EPOLLRDHUP);
  return true;
}

void HttpServer::close(int fd) {
  loop_->remove(fd);
  connections_.erase(fd);
}

}  // namespace softswitch::gui
