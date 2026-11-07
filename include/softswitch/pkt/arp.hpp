#pragma once

/// @file
/// ARP for IPv4 over Ethernet (RFC 826).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

inline constexpr std::size_t kArpLen = 28;
inline constexpr std::uint16_t kArpOpRequest = 1;
inline constexpr std::uint16_t kArpOpReply = 2;

struct ArpPacket {
  std::uint16_t op = 0;
  MacAddress sender_mac;
  Ipv4Address sender_ip;
  MacAddress target_mac;
  Ipv4Address target_ip;
};

/// Parses an Ethernet/IPv4 ARP payload (the bytes after the EtherType).
[[nodiscard]] std::optional<ArpPacket> parse_arp(std::span<const std::uint8_t> payload) noexcept;

/// Builds an untagged Ethernet frame carrying @p arp, padded to the minimum frame size.
/// Returns the frame length, or 0 if @p out is too small.
std::size_t build_arp_frame(std::span<std::uint8_t> out, const MacAddress& dst,
                            const MacAddress& src, const ArpPacket& arp) noexcept;

}  // namespace softswitch::pkt
