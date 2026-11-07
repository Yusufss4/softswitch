#pragma once

/// @file
/// IPv4 header parsing, checksums (RFC 1071) and incremental updates (RFC 1624).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

inline constexpr std::size_t kIpv4MinHeaderLen = 20;
inline constexpr std::uint8_t kIpProtoIcmp = 1;
inline constexpr std::uint8_t kIpProtoIgmp = 2;
inline constexpr std::uint8_t kIpProtoTcp = 6;
inline constexpr std::uint8_t kIpProtoUdp = 17;

struct Ipv4Header {
  std::uint8_t header_len = kIpv4MinHeaderLen;  ///< In bytes (IHL * 4).
  std::uint8_t tos = 0;
  std::uint16_t total_len = 0;
  std::uint16_t id = 0;
  std::uint16_t flags_frag = 0;
  std::uint8_t ttl = 0;
  std::uint8_t protocol = 0;
  std::uint16_t checksum = 0;
  Ipv4Address src;
  Ipv4Address dst;
};

/// Parses and sanity-checks an IPv4 header at the start of @p l3 (version, IHL, total length).
/// The checksum is not verified; use ipv4_header_ok().
[[nodiscard]] std::optional<Ipv4Header> parse_ipv4(std::span<const std::uint8_t> l3) noexcept;

/// True if the header checksum of the IPv4 header at the start of @p l3 is valid.
[[nodiscard]] bool ipv4_header_ok(std::span<const std::uint8_t> l3) noexcept;

/// One's-complement Internet checksum over @p data (RFC 1071).
[[nodiscard]] std::uint16_t internet_checksum(std::span<const std::uint8_t> data,
                                              std::uint32_t initial = 0) noexcept;

/// Incrementally updates @p checksum after a 16-bit word changed from @p old_word to
/// @p new_word (RFC 1624, eqn. 3).
[[nodiscard]] std::uint16_t checksum_adjust(std::uint16_t checksum, std::uint16_t old_word,
                                            std::uint16_t new_word) noexcept;

/// Decrements the TTL of the IPv4 header at the start of @p l3 and patches its checksum
/// incrementally. Returns the new TTL. The caller must have validated the header.
std::uint8_t decrement_ttl(std::span<std::uint8_t> l3) noexcept;

/// TCP/UDP checksum over the IPv4 pseudo-header and @p l4 (the whole segment).
[[nodiscard]] std::uint16_t l4_checksum(Ipv4Address src, Ipv4Address dst, std::uint8_t protocol,
                                        std::span<const std::uint8_t> l4) noexcept;

/// Computes the TCP or UDP checksum of the untagged Ethernet frame @p frame in place.
/// Frames from a sender that offloaded checksumming (veth, virtio) arrive with only the
/// pseudo-header sum filled in (AF_PACKET reports TP_STATUS_CSUMNOTREADY); a switch must
/// finish them before they leave. Returns false for anything that isn't IPv4 TCP/UDP.
bool complete_l4_checksum(std::span<std::uint8_t> frame) noexcept;

/// Writes a 20-byte header (no options) with a valid checksum. Returns 20, or 0 if too small.
std::size_t write_ipv4_header(std::span<std::uint8_t> out, const Ipv4Header& header) noexcept;

}  // namespace softswitch::pkt
