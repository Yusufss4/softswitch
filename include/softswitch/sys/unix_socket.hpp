#pragma once

/// @file
/// Unix-domain sockets and file-descriptor passing (SCM_RIGHTS).

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "softswitch/sys/fd.hpp"

namespace softswitch::sys {

/// Maximum descriptors passed in one message.
inline constexpr std::size_t kMaxPassedFds = 8;

/// Binds and listens on @p path (an existing socket file is replaced).
/// @p type is SOCK_STREAM or SOCK_SEQPACKET.
[[nodiscard]] UniqueFd unix_listen(const std::string& path, int type, int backlog = 8);

/// Connects to @p path. Throws on failure.
[[nodiscard]] UniqueFd unix_connect(const std::string& path, int type);

/// Sends @p payload (at least one byte) with @p fds attached as SCM_RIGHTS.
void send_with_fds(int sock, std::span<const std::byte> payload, std::span<const int> fds);

/// Receives one message. Passed descriptors are appended to @p fds.
/// Returns the payload size; 0 means the peer closed the connection.
[[nodiscard]] std::size_t recv_with_fds(int sock, std::span<std::byte> payload,
                                        std::vector<UniqueFd>& fds);

}  // namespace softswitch::sys
