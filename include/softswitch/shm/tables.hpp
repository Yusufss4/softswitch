#pragma once

/// @file
/// Forwarding tables the control plane publishes and the data plane reads.
///
/// Everything here is plain data with fixed capacity, so a whole `Tables` copy can live in
/// shared memory and be rebuilt by the control plane while the data plane reads the other copy.
/// Hash tables use open addressing with linear probing and a load factor capped at 50%; the
/// control plane rebuilds a copy from scratch on every publish, so there are no deletions or
/// tombstones.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "softswitch/common/types.hpp"

namespace softswitch::shm {

inline constexpr std::size_t kIfNameLen = 16;
inline constexpr std::size_t kFdbSlots = 16384;
inline constexpr std::size_t kMdbSlots = 2048;
inline constexpr std::size_t kNeighborSlots = 2048;
inline constexpr std::size_t kMaxRoutes = 64;
inline constexpr std::size_t kMaxSvis = 16;

enum class StpState : std::uint8_t {
  kDisabled = 0,
  kDiscarding = 1,
  kLearning = 2,
  kForwarding = 3
};
enum class PortMode : std::uint8_t { kAccess = 0, kTrunk = 1 };

namespace port_flag {
inline constexpr std::uint8_t kAdminUp = 0x01;
inline constexpr std::uint8_t kOperUp = 0x02;
inline constexpr std::uint8_t kEdge = 0x04;
}  // namespace port_flag

struct PortEntry {
  std::array<char, kIfNameLen> name{};
  std::int32_t ifindex = 0;
  MacAddress mac;
  std::uint8_t flags = 0;
  PortMode mode = PortMode::kAccess;
  StpState stp_state = StpState::kForwarding;
  VlanId pvid = 1;  ///< VLAN for untagged ingress; 0 drops untagged frames.

  [[nodiscard]] constexpr bool up() const noexcept {
    return (flags & (port_flag::kAdminUp | port_flag::kOperUp)) ==
           (port_flag::kAdminUp | port_flag::kOperUp);
  }
};

namespace vlan_flag {
inline constexpr std::uint8_t kActive = 0x01;
inline constexpr std::uint8_t kIgmpSnooping = 0x02;
inline constexpr std::uint8_t kHasSvi = 0x04;
}  // namespace vlan_flag

struct VlanEntry {
  PortMask members = 0;   ///< Ports allowed to send and receive in this VLAN.
  PortMask untagged = 0;  ///< Members that transmit this VLAN without a tag.
  PortMask mrouters = 0;  ///< Ports facing multicast routers (IGMP snooping).
  std::uint8_t flags = 0;
  std::uint8_t svi_index = 0;  ///< Index into Tables::svis when kHasSvi is set.
};

namespace slot_flag {
inline constexpr std::uint8_t kUsed = 0x01;
inline constexpr std::uint8_t kStatic = 0x02;
}  // namespace slot_flag

struct FdbSlot {
  MacAddress mac;
  VlanId vlan = 0;
  PortId port = kInvalidPort;
  std::uint8_t flags = 0;
};

struct MdbSlot {
  Ipv4Address group;
  VlanId vlan = 0;
  std::uint8_t flags = 0;
  PortMask ports = 0;
};

struct NeighborSlot {
  Ipv4Address ip;
  MacAddress mac;
  VlanId vlan = 0;
  std::uint8_t flags = 0;
};

namespace route_flag {
inline constexpr std::uint8_t kConnected = 0x01;
}

struct RouteEntry {
  Ipv4Prefix prefix;
  std::uint8_t flags = 0;
  VlanId vlan = 0;      ///< Egress VLAN (the SVI the next hop is reached through).
  Ipv4Address nexthop;  ///< Unused for connected routes (the destination is the next hop).
};

struct SviEntry {
  VlanId vlan = 0;
  Ipv4Prefix address;  ///< Interface address, e.g. 10.0.10.1/24.
};

namespace global_flag {
inline constexpr std::uint8_t kStpEnabled = 0x01;
inline constexpr std::uint8_t kRoutingEnabled = 0x02;
}  // namespace global_flag

/// One complete, self-consistent snapshot of every data-plane table.
struct Tables {
  std::uint64_t generation = 0;
  std::uint32_t num_ports = 0;
  std::uint32_t trace_sample = 0;  ///< Record 1 in N packets for the GUI trace; 0 = off.
  std::uint8_t flags = 0;
  MacAddress router_mac;
  std::array<PortEntry, kMaxPorts> ports{};
  std::array<VlanEntry, kVlanTableSize> vlans{};
  std::array<FdbSlot, kFdbSlots> fdb{};
  std::array<MdbSlot, kMdbSlots> mdb{};
  std::array<NeighborSlot, kNeighborSlots> neighbors{};
  std::array<SviEntry, kMaxSvis> svis{};
  std::uint32_t num_svis = 0;
  std::array<RouteEntry, kMaxRoutes> routes{};  ///< Sorted by prefix length, longest first.
  std::uint32_t num_routes = 0;
  std::uint32_t fdb_count = 0;
  std::uint32_t mdb_count = 0;
  std::uint32_t neighbor_count = 0;
};
static_assert(std::is_trivially_copyable_v<Tables>);

// ---- hashing -------------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint64_t fdb_hash(const MacAddress& mac, VlanId vlan) noexcept {
  return mix64(mac.to_u64() ^ (std::uint64_t{vlan} << 48U));
}
[[nodiscard]] constexpr std::uint64_t mdb_hash(Ipv4Address group, VlanId vlan) noexcept {
  return mix64((std::uint64_t{vlan} << 32U) | group.value);
}
[[nodiscard]] constexpr std::uint64_t neighbor_hash(Ipv4Address ip) noexcept {
  return mix64(ip.value);
}

// ---- lookups (data plane; read-only) --------------------------------------------------------

[[nodiscard]] const FdbSlot* fdb_lookup(const Tables& t, const MacAddress& mac,
                                        VlanId vlan) noexcept;
[[nodiscard]] const MdbSlot* mdb_lookup(const Tables& t, Ipv4Address group, VlanId vlan) noexcept;
[[nodiscard]] const NeighborSlot* neighbor_lookup(const Tables& t, Ipv4Address ip) noexcept;
/// Longest-prefix match. Returns nullptr if no route covers @p dst.
[[nodiscard]] const RouteEntry* route_lookup(const Tables& t, Ipv4Address dst) noexcept;

// ---- building (control plane) ---------------------------------------------------------------

/// Empties the hash tables and routes; ports, VLANs and globals are left alone.
void clear_dynamic(Tables& t) noexcept;
/// Inserts or overwrites. Returns false if the table is at its load-factor limit.
bool fdb_insert(Tables& t, const MacAddress& mac, VlanId vlan, PortId port,
                bool is_static) noexcept;
bool mdb_insert(Tables& t, Ipv4Address group, VlanId vlan, PortMask ports) noexcept;
bool neighbor_insert(Tables& t, Ipv4Address ip, const MacAddress& mac, VlanId vlan) noexcept;
/// Adds a route keeping the longest-prefix-first order. Returns false if full.
bool route_insert(Tables& t, const RouteEntry& route) noexcept;

}  // namespace softswitch::shm
