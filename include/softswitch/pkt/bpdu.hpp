#pragma once

/// @file
/// Spanning-tree BPDUs (IEEE 802.1D-2004 §9): configuration, TCN and RST BPDUs.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

/// Bridge group address all spanning-tree BPDUs are sent to.
inline constexpr MacAddress kStpGroupMac{{0x01, 0x80, 0xC2, 0x00, 0x00, 0x00}};

inline constexpr std::uint8_t kBpduTypeConfig = 0x00;
inline constexpr std::uint8_t kBpduTypeTcn = 0x80;
inline constexpr std::uint8_t kBpduTypeRst = 0x02;
inline constexpr std::uint8_t kBpduVersionStp = 0;
inline constexpr std::uint8_t kBpduVersionRstp = 2;

namespace bpdu_flag {
inline constexpr std::uint8_t kTopologyChange = 0x01;
inline constexpr std::uint8_t kProposal = 0x02;
inline constexpr std::uint8_t kRoleMask = 0x0C;
inline constexpr std::uint8_t kLearning = 0x10;
inline constexpr std::uint8_t kForwarding = 0x20;
inline constexpr std::uint8_t kAgreement = 0x40;
inline constexpr std::uint8_t kTopologyChangeAck = 0x80;
}  // namespace bpdu_flag

/// Port role as encoded in RST BPDU flags (bits 2-3).
enum class BpduRole : std::uint8_t {
  kUnknown = 0,
  kAlternateBackup = 1,
  kRoot = 2,
  kDesignated = 3
};

struct Bpdu {
  std::uint8_t version = kBpduVersionRstp;
  std::uint8_t type = kBpduTypeRst;
  std::uint8_t flags = 0;
  std::uint64_t root_id = 0;  ///< Priority (16 bits) + MAC (48 bits).
  std::uint32_t root_path_cost = 0;
  std::uint64_t bridge_id = 0;
  std::uint16_t port_id = 0;  ///< Priority (4 bits) + port number (12 bits).
  // Timers in 1/256 s, as on the wire.
  std::uint16_t message_age = 0;
  std::uint16_t max_age = 0;
  std::uint16_t hello_time = 0;
  std::uint16_t forward_delay = 0;

  [[nodiscard]] constexpr BpduRole role() const noexcept {
    return static_cast<BpduRole>((flags & bpdu_flag::kRoleMask) >> 2U);
  }
  constexpr void set_role(BpduRole role) noexcept {
    const unsigned keep = flags & (0xFFU ^ bpdu_flag::kRoleMask);
    const unsigned bits = (static_cast<unsigned>(role) << 2U) & bpdu_flag::kRoleMask;
    flags = static_cast<std::uint8_t>(keep | bits);
  }
  [[nodiscard]] constexpr bool has(std::uint8_t flag) const noexcept { return (flags & flag) != 0; }
};

[[nodiscard]] constexpr std::uint16_t seconds_to_bpdu_time(unsigned seconds) noexcept {
  return static_cast<std::uint16_t>(seconds * 256U);
}

/// Parses a BPDU from an untagged frame sent to kStpGroupMac (802.3 length + LLC 42 42 03).
[[nodiscard]] std::optional<Bpdu> parse_bpdu_frame(std::span<const std::uint8_t> frame) noexcept;

/// Builds an untagged BPDU frame from @p src. Returns the frame length, or 0 if too small.
std::size_t build_bpdu_frame(std::span<std::uint8_t> out, const MacAddress& src,
                             const Bpdu& bpdu) noexcept;

}  // namespace softswitch::pkt
