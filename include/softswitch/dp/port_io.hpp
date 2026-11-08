#pragma once

/// @file
/// Packet I/O abstraction for one switch port.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "softswitch/dp/frame.hpp"

namespace softswitch::dp {

/// One port's receive and transmit path. Implementations: AfPacketPort (TPACKET_V2 rings).
/// An AF_XDP implementation could slot in here on kernels built with CONFIG_XDP_SOCKETS.
class PortIo {
 public:
  PortIo() = default;
  virtual ~PortIo() = default;
  PortIo(const PortIo&) = delete;
  PortIo& operator=(const PortIo&) = delete;
  PortIo(PortIo&&) = delete;
  PortIo& operator=(PortIo&&) = delete;

  /// Descriptor to poll() for POLLIN when no frames are ready.
  [[nodiscard]] virtual int poll_fd() const noexcept = 0;
  /// True if at least one received frame is waiting (checked without a syscall).
  [[nodiscard]] virtual bool rx_ready() const noexcept = 0;
  /// Fills @p out with up to out.size() ready frames. They stay valid until rx_release().
  virtual std::size_t rx_burst(std::span<RxFrame> out) noexcept = 0;
  /// Returns the frames from the last rx_burst() to the kernel.
  virtual void rx_release() noexcept = 0;
  /// Queues a frame (inserting an 802.1Q tag if @p tci is set). Returns false if the TX ring
  /// is full; the frame is dropped and counted.
  virtual bool tx(std::span<const std::uint8_t> untagged,
                  std::optional<std::uint16_t> tci) noexcept = 0;
  /// Kicks the kernel to send everything queued since the last flush.
  virtual void tx_flush() noexcept = 0;
  [[nodiscard]] virtual std::uint64_t tx_ring_full() const noexcept = 0;
  /// Reads and so clears a pending socket error (e.g. ENETDOWN after the link went down). Call
  /// it when poll() reports POLLERR for poll_fd(): until the error is read, poll() keeps
  /// returning at once and the caller would never sleep.
  virtual void clear_error() noexcept {}
  /// Cumulative frames dropped by the kernel before reaching the RX ring. May cost a syscall,
  /// so callers sample it rather than calling it per batch.
  [[nodiscard]] virtual std::uint64_t rx_kernel_drops() noexcept { return 0; }
};

}  // namespace softswitch::dp
