#pragma once

/// @file
/// IGMP v1/v2/v3 messages (RFC 1112, 2236, 3376) as needed for snooping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::pkt {

inline constexpr std::uint8_t kIgmpQuery = 0x11;
inline constexpr std::uint8_t kIgmpV1Report = 0x12;
inline constexpr std::uint8_t kIgmpV2Report = 0x16;
inline constexpr std::uint8_t kIgmpV2Leave = 0x17;
inline constexpr std::uint8_t kIgmpV3Report = 0x22;

/// IGMPv3 group record types (RFC 3376 §4.2.12).
inline constexpr std::uint8_t kIgmpModeIsInclude = 1;
inline constexpr std::uint8_t kIgmpModeIsExclude = 2;
inline constexpr std::uint8_t kIgmpChangeToInclude = 3;
inline constexpr std::uint8_t kIgmpChangeToExclude = 4;
inline constexpr std::uint8_t kIgmpAllowNewSources = 5;
inline constexpr std::uint8_t kIgmpBlockOldSources = 6;

inline constexpr Ipv4Address kAllHostsGroup = Ipv4Address::from_octets(224, 0, 0, 1);
inline constexpr std::size_t kMaxIgmpRecords = 16;

struct IgmpGroupRecord {
  std::uint8_t type = 0;
  std::uint16_t num_sources = 0;
  Ipv4Address group;
};

struct IgmpMessage {
  std::uint8_t type = 0;
  std::uint8_t max_resp = 0;  ///< Queries: max response time (1/10 s, v2 encoding).
  Ipv4Address group;          ///< v1/v2 reports, leaves and group-specific queries.
  std::array<IgmpGroupRecord, kMaxIgmpRecords> records{};  ///< v3 reports.
  std::size_t num_records = 0;  ///< v3 reports; records beyond kMaxIgmpRecords are dropped.

  /// True if a v3 group record means the sender is (still) a member of the group.
  [[nodiscard]] static bool record_is_join(const IgmpGroupRecord& r) noexcept;
};

/// Parses an IGMP message (the IPv4 payload) and verifies its checksum.
[[nodiscard]] std::optional<IgmpMessage> parse_igmp(std::span<const std::uint8_t> l4) noexcept;

/// Builds an untagged IGMPv2 membership query frame (general if @p group is 0.0.0.0),
/// with TTL 1 and the Router Alert option. Returns the frame length, or 0 if too small.
std::size_t build_igmp_query_frame(std::span<std::uint8_t> out, const MacAddress& src_mac,
                                   Ipv4Address src_ip, Ipv4Address group,
                                   std::uint8_t max_resp_tenths) noexcept;

}  // namespace softswitch::pkt
