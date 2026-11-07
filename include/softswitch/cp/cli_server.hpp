#pragma once

/// @file
/// The CLI socket: a SOCK_STREAM Unix socket served from the control-plane event loop.
///
/// Protocol: the client sends one command per line; the server answers each line with the
/// command output followed by a single NUL byte. Output never contains NUL.

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "softswitch/cp/event_loop.hpp"
#include "softswitch/sys/fd.hpp"

namespace softswitch::cp {

class CliServer {
 public:
  using Handler = std::function<std::string(const std::string& line)>;

  CliServer(EventLoop& loop, std::string path, Handler handler);
  ~CliServer();
  CliServer(const CliServer&) = delete;
  CliServer& operator=(const CliServer&) = delete;
  CliServer(CliServer&&) = delete;
  CliServer& operator=(CliServer&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  struct Session {
    sys::UniqueFd fd;
    std::string in;
    std::string out;
  };

  void accept_client();
  void on_event(int fd, std::uint32_t events);
  /// Writes pending output. Returns false if the session was closed (and freed).
  bool flush(Session& session);
  void close_session(int fd);

  EventLoop* loop_;
  std::string path_;
  Handler handler_;
  sys::UniqueFd listener_;
  std::unordered_map<int, std::unique_ptr<Session>> sessions_;
};

}  // namespace softswitch::cp
