#pragma once

/// @file
/// Minimal HTTP/1.1 for the read-only GUI: request-head parsing (pure) and a server that runs in
/// the control-plane event loop, including Server-Sent Events.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "softswitch/cp/event_loop.hpp"
#include "softswitch/sys/fd.hpp"

namespace softswitch::gui {

inline constexpr std::size_t kMaxRequestHead = 8192;
inline constexpr std::size_t kMaxSseBacklog =
    std::size_t{256} * 1024;  ///< Per client; beyond it events drop.

struct HttpRequest {
  std::string method;
  std::string path;   ///< Without the query string.
  std::string query;  ///< After '?', undecoded.
};

/// Parses the request line of a complete request head ("...\r\n\r\n"). Returns nullopt for
/// anything malformed.
[[nodiscard]] std::optional<HttpRequest> parse_request_head(std::string_view head);

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
};

/// Serialises a complete response (Connection: close, CORS open, no caching).
[[nodiscard]] std::string render_response(const HttpResponse& response);

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  /// Listens on @p bind:@p port (port 0 = any free port).
  HttpServer(cp::EventLoop& loop, const std::string& bind, std::uint16_t port);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  /// Serves @p path (exact match) with @p handler.
  void route(const std::string& path, Handler handler);
  /// Fallback for paths without a route (e.g. static assets).
  void fallback(Handler handler) { fallback_ = std::move(handler); }
  /// Requests to @p path become Server-Sent Events subscriptions.
  void events_at(const std::string& path) { events_path_ = path; }
  /// Sends one event to every subscriber; slow subscribers lose it (counted).
  void broadcast(std::string_view event, std::string_view data);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::size_t subscribers() const noexcept;
  [[nodiscard]] std::uint64_t dropped_events() const noexcept { return dropped_events_; }

 private:
  struct Connection {
    sys::UniqueFd fd;
    std::string in;
    std::string out;
    bool streaming = false;  ///< SSE subscriber: never closed by us.
    bool closing = false;    ///< Close once `out` is flushed.
  };

  void accept_clients();
  void on_event(int fd, std::uint32_t events);
  void handle(Connection& c, const HttpRequest& request);
  /// Returns false if the connection was closed.
  bool flush(Connection& c);
  void close(int fd);

  cp::EventLoop* loop_;
  sys::UniqueFd listener_;
  std::uint16_t port_ = 0;
  std::map<std::string, Handler, std::less<>> routes_;
  Handler fallback_;
  std::string events_path_;
  std::map<int, std::unique_ptr<Connection>> connections_;
  std::uint64_t dropped_events_ = 0;
};

}  // namespace softswitch::gui
