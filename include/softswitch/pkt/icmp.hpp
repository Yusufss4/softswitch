#pragma once

/// @file
/// ICMPv4 echo and error messages (RFC 792).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

inline constexpr std::size_t kIcmpHeaderLen = 8;
inline constexpr std::uint8_t kIcmpEchoReply = 0;
inline constexpr std::uint8_t kIcmpDestUnreachable = 3;
inline constexpr std::uint8_t kIcmpEchoRequest = 8;
inline constexpr std::uint8_t kIcmpTimeExceeded = 11;

struct IcmpHeader {
  std::uint8_t type = 0;
  std::uint8_t code = 0;
  std::uint16_t checksum = 0;
  std::uint16_t id = 0;   ///< Echo only.
  std::uint16_t seq = 0;  ///< Echo only.
};

[[nodiscard]] std::optional<IcmpHeader> parse_icmp(std::span<const std::uint8_t> l4) noexcept;

/// Turns an untagged Ethernet frame holding an ICMP echo request into the reply, in place:
/// swaps MAC and IP addresses, sets TTL 64 and recomputes both checksums.
/// Returns false (frame unchanged) if it isn't a well-formed echo request.
[[nodiscard]] bool echo_request_to_reply(std::span<std::uint8_t> frame) noexcept;

/// Builds an untagged ICMP error frame (e.g. time exceeded) from @p from to the sender of
/// @p offending (an IPv4 packet), quoting its header plus 8 bytes as RFC 792 requires.
/// Returns the frame length, or 0 if @p offending is not IPv4 or @p out is too small.
std::size_t build_icmp_error_frame(std::span<std::uint8_t> out, const MacAddress& dst,
                                   const MacAddress& src, Ipv4Address from, std::uint8_t type,
                                   std::uint8_t code,
                                   std::span<const std::uint8_t> offending) noexcept;

}  // namespace softswitch::pkt
