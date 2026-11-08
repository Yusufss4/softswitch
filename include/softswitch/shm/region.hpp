#pragma once

/// @file
/// Layout of the shared-memory region between softswitchd (control plane) and
/// ss-dataplane (data plane), and the messages that flow through its rings.
///
/// Ownership: every field has exactly one writer.
///   control plane writes: `tables[inactive]`, `active`, `publish_seq`, the inject ring (producer)
///   data plane writes:    `dp`, the learn/punt/trace rings (producer)

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "softswitch/common/types.hpp"
#include "softswitch/shm/spsc_ring.hpp"
#include "softswitch/shm/tables.hpp"

namespace softswitch::shm {

inline constexpr std::uint64_t kRegionMagic = 0x3148435449575353ULL;  // "SSWITCH1"
inline constexpr std::uint32_t kRegionVersion = 2;  ///< Bump on any layout change.
inline constexpr std::size_t kPacketDataLen = 1536;
inline constexpr std::size_t kHitBits = 65536;  ///< FDB aging bitmap, indexed by fdb_hash.

// ---- messages -------------------------------------------------------------------------------

/// Data plane saw a source MAC it doesn't know, or on a different port (MAC move).
struct LearnEvent {
  MacAddress mac;
  VlanId vlan = 0;
  PortId port = kInvalidPort;
};

enum class PuntReason : std::uint8_t {
  kNone = 0,
  kBpdu,          ///< Spanning tree.
  kArp,           ///< ARP in a VLAN with an SVI.
  kIgmp,          ///< IGMP for snooping (a copy; the frame is still forwarded).
  kToRouter,      ///< IPv4 addressed to one of the switch's own SVI addresses.
  kNeighborMiss,  ///< Routable, but the next hop's MAC is unknown.
  kTtlExpired,    ///< Routed packet whose TTL would reach zero.
};

namespace inject_flag {
/// Send exactly to `port`, untagged, ignoring VLAN and STP state (e.g. BPDUs).
inline constexpr std::uint8_t kRaw = 0x01;
/// Forward within `vlan` as if received from the CPU: FDB lookup, flood on miss,
/// per-port tagging, STP state respected. `port` is ignored.
inline constexpr std::uint8_t kVlan = 0x02;
}  // namespace inject_flag

/// A frame moving between the planes. `data` always holds an untagged frame; the VLAN travels
/// in `vlan` and is (re)applied per egress port.
struct PacketMessage {
  PortId port = kInvalidPort;
  VlanId vlan = 0;
  std::uint16_t len = 0;
  PuntReason reason = PuntReason::kNone;
  std::uint8_t flags = 0;
  std::uint64_t timestamp_ns = 0;
  std::array<std::uint8_t, kPacketDataLen> data{};
};

enum class Decision : std::uint8_t {
  kForward = 0,  ///< Known unicast to one port.
  kFlood,        ///< Unknown unicast / broadcast / unregistered multicast.
  kMulticast,    ///< Snooped multicast to member + mrouter ports.
  kRouted,       ///< Routed between VLANs.
  kPunt,         ///< Sent to the control plane only.
  kDrop,
};

enum class DropReason : std::uint8_t {
  kNone = 0,
  kPortDown,
  kMalformed,
  kUntaggedNotAllowed,
  kTaggedOnAccess,
  kVlanNotMember,
  kStpBlocked,
  kReservedMulticast,
  kSameSegment,  ///< Destination is on the ingress port.
  kNoEgress,     ///< Every candidate egress port was filtered out.
  kNoRoute,
  kPuntRingFull,
  kNoListeners,  ///< Snooped multicast with neither members nor mrouter ports.
  kCount,
};
inline constexpr std::size_t kDropReasonCount = static_cast<std::size_t>(DropReason::kCount);

[[nodiscard]] const char* to_string(Decision decision) noexcept;
[[nodiscard]] const char* to_string(DropReason reason) noexcept;
[[nodiscard]] const char* to_string(StpState state) noexcept;

/// One sampled forwarding decision, for the GUI and `show trace`.
struct TraceRecord {
  std::uint64_t timestamp_ns = 0;
  PortId in_port = kInvalidPort;
  VlanId vlan = 0;
  VlanId out_vlan = 0;
  Decision decision = Decision::kDrop;
  DropReason reason = DropReason::kNone;
  PortMask out_ports = 0;
  MacAddress dst;
  MacAddress src;
  std::uint16_t ethertype = 0;
  std::uint8_t ip_proto = 0;
  Ipv4Address src_ip;
  Ipv4Address dst_ip;
  std::uint16_t len = 0;
};

// ---- data-plane state -----------------------------------------------------------------------

struct alignas(kCacheLine) PortCounters {
  std::atomic<std::uint64_t> rx_packets{0};
  std::atomic<std::uint64_t> rx_bytes{0};
  std::atomic<std::uint64_t> tx_packets{0};
  std::atomic<std::uint64_t> tx_bytes{0};
  std::atomic<std::uint64_t> drops{0};
};

/// Written only by the data plane. Counters are single-writer, so relaxed increments
/// (load + store) are enough; readers may see slightly stale values.
struct DataPlaneState {
  alignas(kCacheLine) std::atomic<std::uint64_t> ack_seq{0};  ///< Last publish_seq fully used.
  std::atomic<std::uint32_t> idle{1};      ///< 1 while blocked in poll(), holding no table.
  std::atomic<std::uint32_t> attached{0};  ///< 1 while the data plane process is running.
  std::atomic<std::uint64_t> heartbeat_ns{0};
  std::atomic<std::int32_t> pid{0};
  alignas(kCacheLine) std::atomic<std::uint64_t> batches{0};
  std::atomic<std::uint64_t> rx_packets{0};
  std::atomic<std::uint64_t> tx_packets{0};
  std::atomic<std::uint64_t> learn_events{0};
  std::atomic<std::uint64_t> punts{0};
  std::atomic<std::uint64_t> injected{0};
  std::atomic<std::uint64_t> max_batch_ns{0};
  std::array<PortCounters, kMaxPorts> ports{};
  std::array<std::atomic<std::uint64_t>, kDropReasonCount> drops{};
  std::array<std::atomic<std::uint64_t>, kHitBits / 64> fdb_hits{};  ///< Set by DP, cleared by CP.
};

/// Bumps a counter that has a single writer without a locked read-modify-write.
inline void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

// ---- the region -----------------------------------------------------------------------------

struct SharedRegion {
  std::uint64_t magic = kRegionMagic;
  std::uint32_t version = kRegionVersion;
  std::uint32_t size = sizeof(SharedRegion);

  // Control plane -> data plane: which Tables copy is live, and how many times it flipped.
  alignas(kCacheLine) std::atomic<std::uint32_t> active{0};
  std::atomic<std::uint64_t> publish_seq{0};
  alignas(kCacheLine) std::array<Tables, 2> tables{};

  DataPlaneState dp{};

  SpscRing<LearnEvent, 4096> learn{};     ///< DP -> CP
  SpscRing<PacketMessage, 256> punt{};    ///< DP -> CP
  SpscRing<PacketMessage, 256> inject{};  ///< CP -> DP
  SpscRing<TraceRecord, 8192> trace{};    ///< DP -> CP (lossy by design)
};

}  // namespace softswitch::shm
