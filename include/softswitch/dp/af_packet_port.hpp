#pragma once

/// @file
/// PortIo over AF_PACKET with mmap'd TPACKET_V2 RX and TX rings.
///
/// TPACKET_V2 rather than V3: V3 hands frames to user space in blocks that are only retired
/// when full or when the block timer (>= 1 ms) fires, which adds up to a millisecond of latency
/// per hop at low rates. V2 releases every frame individually.

#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "softswitch/dp/port_io.hpp"
#include "softswitch/sys/fd.hpp"

namespace softswitch::dp {

enum class TxMode : std::uint8_t {
  kRing,      ///< PACKET_TX_RING + one send() per batch.
  kSendto,    ///< One send() syscall per frame (baseline for benchmarks).
  kSendmmsg,  ///< Linear copies staged per batch, one sendmmsg() per flush (default).
};

/// Parses "ring", "sendto" or "sendmmsg".
std::optional<TxMode> parse_tx_mode(std::string_view name) noexcept;

struct AfPacketOptions {
  std::string ifname;
  std::uint32_t frame_size = 2048;
  std::uint32_t rx_frames = 1024;
  std::uint32_t tx_frames = 512;
  TxMode tx_mode = TxMode::kSendmmsg;
};

class AfPacketPort final : public PortIo {
 public:
  /// Opens the RX and TX sockets on @p options.ifname. Needs CAP_NET_RAW. Throws on failure.
  explicit AfPacketPort(const AfPacketOptions& options);
  ~AfPacketPort() override;
  AfPacketPort(const AfPacketPort&) = delete;
  AfPacketPort& operator=(const AfPacketPort&) = delete;
  AfPacketPort(AfPacketPort&&) = delete;
  AfPacketPort& operator=(AfPacketPort&&) = delete;

  [[nodiscard]] int poll_fd() const noexcept override { return rx_fd_.get(); }
  [[nodiscard]] bool rx_ready() const noexcept override;
  std::size_t rx_burst(std::span<RxFrame> out) noexcept override;
  void rx_release() noexcept override;
  bool tx(std::span<const std::uint8_t> untagged,
          std::optional<std::uint16_t> tci) noexcept override;
  void tx_flush() noexcept override;
  [[nodiscard]] std::uint64_t tx_ring_full() const noexcept override { return tx_ring_full_; }

  [[nodiscard]] int ifindex() const noexcept { return ifindex_; }

 private:
  struct Ring {
    std::uint8_t* base = nullptr;
    std::size_t bytes = 0;
    std::uint32_t frames = 0;
    std::uint32_t frame_size = 0;
    std::uint32_t next = 0;
    [[nodiscard]] std::uint8_t* frame(std::uint32_t i) const noexcept {
      return base + (static_cast<std::size_t>(i) * frame_size);
    }
  };

  void setup_ring(int fd, int option, Ring& ring, std::uint32_t frames) const;
  void bind_to(int fd, std::uint16_t protocol) const;

  AfPacketOptions options_;
  int ifindex_ = 0;
  sys::UniqueFd rx_fd_;
  sys::UniqueFd tx_fd_;
  Ring rx_;
  Ring tx_;
  std::uint32_t rx_held_ = 0;     ///< Frames handed out by the last rx_burst().
  std::uint32_t tx_pending_ = 0;  ///< Frames queued since the last flush.
  std::uint64_t tx_ring_full_ = 0;
  std::uint64_t rx_kernel_drops_ = 0;     ///< PACKET_STATISTICS resets on read; accumulated here.
  std::vector<std::uint8_t> tx_scratch_;  ///< kSendto: one frame; kSendmmsg: kMmsgBatch frames.
  /// kSendmmsg: frames staged since the last flush. Sized once in the constructor, so tx() and
  /// tx_flush() never allocate.
  static constexpr std::uint32_t kMmsgBatch = 64;
  std::vector<iovec> tx_iov_;
  std::vector<mmsghdr> tx_msgs_;
};

}  // namespace softswitch::dp
