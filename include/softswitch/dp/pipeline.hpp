#pragma once

/// @file
/// The forwarding pipeline: a pure function from (tables, ingress port, frame) to emitted
/// frames, ring messages and counters. It performs no syscalls and never blocks, so it can be
/// unit-tested and fuzzed without sockets.

#include <array>
#include <cstdint>
#include <span>

#include "softswitch/dp/frame.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/shm/region.hpp"

namespace softswitch::dp {

class Pipeline {
 public:
  explicit Pipeline(shm::SharedRegion& region) noexcept : region_(&region) {}

  /// Processes one received frame.
  void process(const shm::Tables& tables, PortId in_port, const RxFrame& frame,
               Egress& egress) noexcept;

  /// Processes one frame injected by the control plane (see shm::inject_flag).
  void process_inject(const shm::Tables& tables, const shm::PacketMessage& msg,
                      Egress& egress) noexcept;

  /// Current time for this batch (CLOCK_MONOTONIC ns); used to rate-limit learn events.
  void set_now(std::uint64_t now_ns) noexcept { now_ns_ = now_ns; }

  /// A repeated learn event for the same (MAC, VLAN, port) is suppressed for this long, which
  /// covers the time the control plane needs to publish the new entry.
  static constexpr std::uint64_t kLearnRepeatNs = 10'000'000;

  /// True if anything was pushed to the learn or punt ring since the last call;
  /// the data plane then signals the control plane's eventfd once per batch.
  [[nodiscard]] bool take_notify() noexcept {
    const bool notify = notify_;
    notify_ = false;
    return notify;
  }

 private:
  struct Classified {
    std::span<const std::uint8_t> untagged;  ///< The frame without any 802.1Q tag.
    pkt::EthernetView eth;
    VlanId vlan = 0;
    bool was_tagged = false;
  };

  /// Sends @p frame to every port in @p ports, tagging per the VLAN's untagged set.
  void emit_to(const shm::Tables& tables, PortMask ports, VlanId vlan,
               std::span<const std::uint8_t> frame, Egress& egress) noexcept;
  void drop(PortId in_port, shm::DropReason reason) noexcept;
  /// Reports a new or moved source MAC, or marks a known one as hit for aging.
  void learn_source(const shm::Tables& tables, PortId in_port, const Classified& c) noexcept;
  /// Copies the (untagged) frame to the punt ring for the control plane.
  void punt(const shm::Tables& tables, PortId in_port, const Classified& c,
            shm::PuntReason reason) noexcept;
  /// drop() plus a trace record.
  void reject(const shm::Tables& tables, PortId in_port, const Classified& c,
              shm::DropReason reason) noexcept;
  /// IEEE 802.1D reserved group addresses 01:80:C2:00:00:00-0F (BPDUs, LACP, LLDP, ...).
  [[nodiscard]] static bool is_reserved_multicast(const MacAddress& dst) noexcept;
  void trace(const shm::Tables& tables, PortId in_port, const Classified& c, shm::Decision decision,
             shm::DropReason reason, PortMask out_ports, VlanId out_vlan) noexcept;
  [[nodiscard]] static PortMask forwarding_ports(const shm::Tables& tables) noexcept;

  shm::SharedRegion* region_;
  std::array<std::uint8_t, shm::kPacketDataLen>
      scratch_{};  ///< Untagged copy of in-band-tagged frames.
  struct RecentLearn {
    std::uint64_t key = 0;
    std::uint64_t timestamp_ns = 0;
    PortId port = kInvalidPort;
  };
  std::array<RecentLearn, 256> recent_learns_{};  ///< Direct-mapped repeat suppression.
  std::uint64_t now_ns_ = 0;
  std::uint32_t trace_counter_ = 0;
  bool notify_ = false;
};

}  // namespace softswitch::dp
