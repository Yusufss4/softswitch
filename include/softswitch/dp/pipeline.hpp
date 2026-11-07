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
  void trace(const shm::Tables& tables, PortId in_port, const Classified& c, shm::Decision decision,
             shm::DropReason reason, PortMask out_ports, VlanId out_vlan) noexcept;
  [[nodiscard]] static PortMask forwarding_ports(const shm::Tables& tables) noexcept;

  shm::SharedRegion* region_;
  std::array<std::uint8_t, shm::kPacketDataLen>
      scratch_{};  ///< Untagged copy of in-band-tagged frames.
  std::uint32_t trace_counter_ = 0;
  bool notify_ = false;
};

}  // namespace softswitch::dp
