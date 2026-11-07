#pragma once

/// @file
/// A received frame as the pipeline sees it, and the egress interface it emits to.

#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/common/types.hpp"

namespace softswitch::dp {

/// A frame handed up by a port. AF_PACKET usually strips the 802.1Q tag into metadata
/// (`vlan_tci_valid`); frames with an in-band tag are handled too.
struct RxFrame {
  std::span<const std::uint8_t> data;
  std::uint16_t vlan_tci = 0;
  bool vlan_tci_valid = false;
  /// The sender offloaded its TCP/UDP checksum (veth, virtio): only the pseudo-header sum is
  /// present and the switch must complete it before forwarding (TP_STATUS_CSUMNOTREADY).
  bool checksum_incomplete = false;
  std::uint64_t timestamp_ns = 0;
};

/// Where the pipeline sends frames. `untagged` is a complete frame without an 802.1Q tag;
/// a tag with @p tci is inserted on the way out when @p tci has a value.
class Egress {
 public:
  virtual void emit(PortId port, std::span<const std::uint8_t> untagged,
                    std::optional<std::uint16_t> tci) noexcept = 0;

 protected:
  Egress() = default;
  ~Egress() = default;
  Egress(const Egress&) = default;
  Egress& operator=(const Egress&) = default;
  Egress(Egress&&) = default;
  Egress& operator=(Egress&&) = default;
};

}  // namespace softswitch::dp
