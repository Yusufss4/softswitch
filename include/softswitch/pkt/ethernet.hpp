#pragma once

/// @file
/// Ethernet II / 802.3 framing and IEEE 802.1Q VLAN tags.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

inline constexpr std::size_t kMacAddrsLen = 12;    ///< Destination + source address.
inline constexpr std::size_t kEthHeaderLen = 14;   ///< Untagged header.
inline constexpr std::size_t kVlanTagLen = 4;      ///< TPID + TCI.
inline constexpr std::size_t kMinFrameLen = 60;    ///< Minimum frame without FCS.
inline constexpr std::size_t kMaxFrameLen = 1518;  ///< Tagged 1500-byte payload, no FCS.

inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeArp = 0x0806;
inline constexpr std::uint16_t kEtherTypeVlan = 0x8100;
/// Type/length values below this are 802.3 length fields (e.g. BPDUs), not EtherTypes.
inline constexpr std::uint16_t kEtherTypeMin = 0x0600;

[[nodiscard]] constexpr VlanId tci_vid(std::uint16_t tci) noexcept {
  return static_cast<VlanId>(tci & 0x0FFFU);
}
[[nodiscard]] constexpr std::uint8_t tci_pcp(std::uint16_t tci) noexcept {
  return static_cast<std::uint8_t>(tci >> 13U);
}
[[nodiscard]] constexpr std::uint16_t make_tci(VlanId vid, std::uint8_t pcp = 0) noexcept {
  return static_cast<std::uint16_t>((std::uint32_t{pcp & 0x7U} << 13U) | (vid & 0x0FFFU));
}

/// Parsed view of an Ethernet header. Offsets refer to the parsed buffer.
struct EthernetView {
  MacAddress dst;
  MacAddress src;
  bool tagged = false;             ///< An 802.1Q tag follows the source address.
  std::uint16_t tci = 0;           ///< Valid when `tagged`.
  std::uint16_t ethertype = 0;     ///< EtherType, or the 802.3 length when < kEtherTypeMin.
  std::size_t payload_offset = 0;  ///< First byte after the (inner) EtherType.
};

/// Parses the Ethernet header (and one 802.1Q tag, if present). Never reads past @p frame.
[[nodiscard]] std::optional<EthernetView> parse_ethernet(
    std::span<const std::uint8_t> frame) noexcept;

/// Writes dst, src and EtherType. Returns kEthHeaderLen, or 0 if @p out is too small.
std::size_t write_eth_header(std::span<std::uint8_t> out, const MacAddress& dst,
                             const MacAddress& src, std::uint16_t ethertype) noexcept;

/// Copies @p untagged into @p out with an 802.1Q tag inserted after the MAC addresses.
/// Returns the bytes written, or 0 if the input is not a frame or @p out is too small.
std::size_t copy_with_tag(std::span<const std::uint8_t> untagged, std::uint16_t tci,
                          std::span<std::uint8_t> out) noexcept;

/// Inserts an 802.1Q tag in place. @p buf holds a frame of @p len bytes and must have room for
/// kVlanTagLen more. Returns the new length, or 0 on error.
std::size_t insert_tag(std::span<std::uint8_t> buf, std::size_t len, std::uint16_t tci) noexcept;

/// Removes the outer 802.1Q tag in place. Returns the new length, or 0 if the frame isn't tagged.
std::size_t strip_tag(std::span<std::uint8_t> frame) noexcept;

/// Zero-pads a frame of @p len bytes up to kMinFrameLen (bounded by @p buf). Returns the new
/// length.
std::size_t pad_frame(std::span<std::uint8_t> buf, std::size_t len) noexcept;

}  // namespace softswitch::pkt
