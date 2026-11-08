#include "softswitch/dp/af_packet_port.hpp"

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include "softswitch/pkt/ethernet.hpp"

namespace softswitch::dp {
namespace {

// Offset of packet data inside a TX ring frame: TPACKET2_HDRLEN - sizeof(sockaddr_ll), i.e. the
// header rounded up to TPACKET_ALIGNMENT (the kernel macro mixes signed arithmetic).
constexpr std::size_t kTpacketAlignment = 16;
constexpr std::size_t kTxDataOffset =
    (sizeof(tpacket2_hdr) + kTpacketAlignment - 1) & ~(kTpacketAlignment - 1);

tpacket2_hdr* header(std::uint8_t* frame) noexcept {
  return reinterpret_cast<tpacket2_hdr*>(frame);
}

std::uint32_t load_status(const tpacket2_hdr* h) noexcept {
  return __atomic_load_n(&h->tp_status, __ATOMIC_ACQUIRE);
}
void store_status(tpacket2_hdr* h, std::uint32_t status) noexcept {
  __atomic_store_n(&h->tp_status, status, __ATOMIC_RELEASE);
}

void set_int_option(int fd, int level, int option, int value, const char* what) {
  if (::setsockopt(fd, level, option, &value, sizeof(value)) < 0) {
    sys::throw_errno(what);
  }
}

}  // namespace

std::optional<TxMode> parse_tx_mode(std::string_view name) noexcept {
  if (name == "ring") {
    return TxMode::kRing;
  }
  if (name == "sendto") {
    return TxMode::kSendto;
  }
  if (name == "sendmmsg") {
    return TxMode::kSendmmsg;
  }
  return std::nullopt;
}

AfPacketPort::AfPacketPort(const AfPacketOptions& options)
    : options_(options), ifindex_(static_cast<int>(::if_nametoindex(options.ifname.c_str()))) {
  if (ifindex_ == 0) {
    throw std::runtime_error("no such interface: " + options.ifname);
  }

  // RX: every protocol, promiscuous, our own transmissions ignored.
  rx_fd_.reset(::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL)));
  if (!rx_fd_.valid()) {
    sys::throw_errno("socket(AF_PACKET) for " + options.ifname + " (needs CAP_NET_RAW)");
  }
  set_int_option(rx_fd_.get(), SOL_PACKET, PACKET_VERSION, TPACKET_V2, "PACKET_VERSION");
  set_int_option(rx_fd_.get(), SOL_PACKET, PACKET_IGNORE_OUTGOING, 1, "PACKET_IGNORE_OUTGOING");
  setup_ring(rx_fd_.get(), PACKET_RX_RING, rx_, options.rx_frames);
  bind_to(rx_fd_.get(), htons(ETH_P_ALL));
  packet_mreq mreq{};
  mreq.mr_ifindex = ifindex_;
  mreq.mr_type = PACKET_MR_PROMISC;
  if (::setsockopt(rx_fd_.get(), SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    sys::throw_errno("PACKET_ADD_MEMBERSHIP(promisc)");
  }

  // TX: protocol 0 so it receives nothing; bypass the qdisc for lower latency.
  tx_fd_.reset(::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0));
  if (!tx_fd_.valid()) {
    sys::throw_errno("socket(AF_PACKET, tx)");
  }
  set_int_option(tx_fd_.get(), SOL_PACKET, PACKET_VERSION, TPACKET_V2, "PACKET_VERSION");
  set_int_option(tx_fd_.get(), SOL_PACKET, PACKET_QDISC_BYPASS, 1, "PACKET_QDISC_BYPASS");
  if (options.tx_mode == TxMode::kRing) {
    setup_ring(tx_fd_.get(), PACKET_TX_RING, tx_, options.tx_frames);
  } else if (options.tx_mode == TxMode::kSendmmsg) {
    tx_scratch_.resize(static_cast<std::size_t>(kMmsgBatch) * options.frame_size);
    tx_iov_.resize(kMmsgBatch);
    tx_msgs_.resize(kMmsgBatch);
    for (std::uint32_t i = 0; i < kMmsgBatch; ++i) {
      tx_iov_[i].iov_base = tx_scratch_.data() + (static_cast<std::size_t>(i) * options.frame_size);
      tx_msgs_[i] = {};
      tx_msgs_[i].msg_hdr.msg_iov = &tx_iov_[i];
      tx_msgs_[i].msg_hdr.msg_iovlen = 1;
    }
  } else {
    tx_scratch_.resize(options.frame_size);
  }
  bind_to(tx_fd_.get(), 0);
}

AfPacketPort::~AfPacketPort() {
  if (rx_.base != nullptr) {
    ::munmap(rx_.base, rx_.bytes);
  }
  if (tx_.base != nullptr) {
    ::munmap(tx_.base, tx_.bytes);
  }
}

void AfPacketPort::setup_ring(int fd, int option, Ring& ring, std::uint32_t frames) const {
  constexpr std::uint32_t kBlockSize = 1U << 16U;
  const std::uint32_t frames_per_block = kBlockSize / options_.frame_size;
  tpacket_req req{};
  req.tp_block_size = kBlockSize;
  req.tp_frame_size = options_.frame_size;
  req.tp_block_nr = (frames + frames_per_block - 1) / frames_per_block;
  req.tp_frame_nr = req.tp_block_nr * frames_per_block;
  if (::setsockopt(fd, SOL_PACKET, option, &req, sizeof(req)) < 0) {
    sys::throw_errno("PACKET_RX/TX_RING");
  }
  ring.bytes = static_cast<std::size_t>(req.tp_block_size) * req.tp_block_nr;
  void* base = ::mmap(nullptr, ring.bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_LOCKED | MAP_POPULATE, fd, 0);
  if (base == MAP_FAILED) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast): libc macro
    // MAP_LOCKED fails without CAP_IPC_LOCK; retry unlocked (mlockall may still cover it).
    base = ::mmap(nullptr, ring.bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (base == MAP_FAILED) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast): libc macro
      sys::throw_errno("mmap(packet ring)");
    }
  }
  ring.base = static_cast<std::uint8_t*>(base);
  ring.frames = req.tp_frame_nr;
  ring.frame_size = req.tp_frame_size;
  ring.next = 0;
}

void AfPacketPort::bind_to(int fd, std::uint16_t protocol) const {
  sockaddr_ll addr{};
  addr.sll_family = AF_PACKET;
  addr.sll_protocol = protocol;
  addr.sll_ifindex = ifindex_;
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    sys::throw_errno("bind(AF_PACKET) " + options_.ifname);
  }
}

std::uint64_t AfPacketPort::rx_kernel_drops() noexcept {
  tpacket_stats stats{};
  socklen_t len = sizeof(stats);
  if (::getsockopt(rx_fd_.get(), SOL_PACKET, PACKET_STATISTICS, &stats, &len) == 0) {
    rx_kernel_drops_ += stats.tp_drops;
  }
  return rx_kernel_drops_;
}

void AfPacketPort::clear_error() noexcept {
  int error = 0;
  socklen_t len = sizeof(error);
  (void)::getsockopt(rx_fd_.get(), SOL_SOCKET, SO_ERROR, &error, &len);  // reading clears it
}

bool AfPacketPort::rx_ready() const noexcept {
  return (load_status(header(rx_.frame(rx_.next))) & TP_STATUS_USER) != 0;
}

std::size_t AfPacketPort::rx_burst(std::span<RxFrame> out) noexcept {
  std::size_t n = 0;
  std::uint32_t index = rx_.next;
  while (n < out.size() && n < rx_.frames) {
    const tpacket2_hdr* h = header(rx_.frame(index));
    const std::uint32_t status = load_status(h);
    if ((status & TP_STATUS_USER) == 0) {
      break;
    }
    RxFrame& f = out[n++];
    f.data = std::span<const std::uint8_t>(rx_.frame(index) + h->tp_mac, h->tp_snaplen);
    f.vlan_tci_valid = (status & TP_STATUS_VLAN_VALID) != 0;
    f.vlan_tci = f.vlan_tci_valid ? h->tp_vlan_tci : std::uint16_t{0};
    f.checksum_incomplete = (status & TP_STATUS_CSUMNOTREADY) != 0;
    f.timestamp_ns = (static_cast<std::uint64_t>(h->tp_sec) * 1'000'000'000ULL) + h->tp_nsec;
    index = (index + 1) % rx_.frames;
  }
  rx_held_ = static_cast<std::uint32_t>(n);
  return n;
}

void AfPacketPort::rx_release() noexcept {
  for (std::uint32_t i = 0; i < rx_held_; ++i) {
    store_status(header(rx_.frame(rx_.next)), TP_STATUS_KERNEL);
    rx_.next = (rx_.next + 1) % rx_.frames;
  }
  rx_held_ = 0;
}

bool AfPacketPort::tx(std::span<const std::uint8_t> untagged,
                      std::optional<std::uint16_t> tci) noexcept {
  const std::size_t len = untagged.size() + (tci ? pkt::kVlanTagLen : 0);
  if (options_.tx_mode == TxMode::kSendto) {
    std::span<const std::uint8_t> frame = untagged;
    if (tci) {
      if (pkt::copy_with_tag(untagged, *tci, tx_scratch_) == 0) {
        return false;
      }
      frame = std::span<const std::uint8_t>(tx_scratch_.data(), len);
    }
    return ::send(tx_fd_.get(), frame.data(), frame.size(), MSG_DONTWAIT) ==
           static_cast<ssize_t>(frame.size());
  }
  if (options_.tx_mode == TxMode::kSendmmsg) {
    if (len > options_.frame_size) {
      return false;
    }
    if (tx_pending_ == kMmsgBatch) {
      tx_flush();  // batch full: send it and start a new one
    }
    iovec& iov = tx_iov_[tx_pending_];
    const std::span<std::uint8_t> dst(static_cast<std::uint8_t*>(iov.iov_base),
                                      options_.frame_size);
    if (tci) {
      (void)pkt::copy_with_tag(untagged, *tci, dst);
    } else {
      std::copy(untagged.begin(), untagged.end(), dst.begin());
    }
    iov.iov_len = len;
    ++tx_pending_;
    return true;
  }

  std::uint8_t* slot = tx_.frame(tx_.next);
  tpacket2_hdr* h = header(slot);
  const std::uint32_t status = load_status(h);
  if (status != TP_STATUS_AVAILABLE && status != TP_STATUS_WRONG_FORMAT) {
    ++tx_ring_full_;
    return false;
  }
  if (len > tx_.frame_size - kTxDataOffset) {
    return false;
  }
  const std::span<std::uint8_t> dst(slot + kTxDataOffset, tx_.frame_size - kTxDataOffset);
  if (tci) {
    (void)pkt::copy_with_tag(untagged, *tci, dst);
  } else {
    std::copy(untagged.begin(), untagged.end(), dst.begin());
  }
  h->tp_len = static_cast<std::uint32_t>(len);
  store_status(h, TP_STATUS_SEND_REQUEST);
  tx_.next = (tx_.next + 1) % tx_.frames;
  ++tx_pending_;
  return true;
}

void AfPacketPort::tx_flush() noexcept {
  if (tx_pending_ == 0) {
    return;
  }
  if (options_.tx_mode == TxMode::kSendmmsg) {
    // Each message becomes one linear skb (a copy, as with sendto), but the batch costs one
    // syscall. Frames the kernel refuses (e.g. -ENOBUFS) are dropped and counted.
    std::uint32_t sent = 0;
    while (sent < tx_pending_) {
      const int n = ::sendmmsg(tx_fd_.get(), &tx_msgs_[sent], tx_pending_ - sent, MSG_DONTWAIT);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) {
          continue;
        }
        tx_ring_full_ += tx_pending_ - sent;
        break;
      }
      sent += static_cast<std::uint32_t>(n);
    }
    tx_pending_ = 0;
    return;
  }
  // With PACKET_QDISC_BYPASS the kernel walks the ring synchronously inside send().
  (void)::send(tx_fd_.get(), nullptr, 0, MSG_DONTWAIT);
  tx_pending_ = 0;
}

}  // namespace softswitch::dp
