// ss-pktgen: line-rate test traffic over an AF_PACKET TX ring.
//
//   ss-pktgen --iface IF --dst-mac MAC [--src-mac MAC] [--size 64] [--rate PPS|0=max]
//             [--duration S] [--random-src N] [--cpu N]
//
// Frames are Ethernet/IPv4/UDP to port 9999 carrying {magic, sequence, CLOCK_MONOTONIC ns} as three
// 32-bit fields (so even 64-byte frames fit), so
// ss-sink can count loss and measure one-way latency on the same machine (CLOCK_MONOTONIC is
// shared by every network namespace). Prints one JSON line with what was sent.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "softswitch/common/args.hpp"
#include "softswitch/dp/af_packet_port.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/ipv4.hpp"
#include "softswitch/sys/realtime.hpp"

namespace {

using namespace softswitch;

constexpr std::uint32_t kMagic = 0x53535747;  // "SSWG"
constexpr std::size_t kPayloadAt = 14 + 20 + 8;

struct Options {
  std::string iface;
  MacAddress dst;
  MacAddress src = *MacAddress::parse("02:00:00:00:be:01");
  Ipv4Address src_ip = *Ipv4Address::parse("10.99.0.1");
  Ipv4Address dst_ip = *Ipv4Address::parse("10.99.0.2");
  std::size_t size = 64;  ///< Frame size including the 4-byte FCS the wire would add.
  std::uint64_t rate = 0;
  double duration = 5.0;
  std::uint32_t random_src = 0;
  int cpu = -1;
};

std::vector<std::uint8_t> build_template(const Options& o) {
  const std::size_t len = std::max<std::size_t>(o.size, 64) - 4;  // FCS is added by the "wire"
  std::vector<std::uint8_t> f(len, 0);
  pkt::write_eth_header(f, o.dst, o.src, pkt::kEtherTypeIpv4);
  pkt::Ipv4Header h;
  h.total_len = static_cast<std::uint16_t>(len - 14);
  h.ttl = 64;
  h.protocol = pkt::kIpProtoUdp;
  h.src = o.src_ip;
  h.dst = o.dst_ip;
  pkt::write_ipv4_header(std::span{f}.subspan(14), h);
  std::uint8_t* udp = f.data() + 34;
  pkt::store_be16(udp, 9998);
  pkt::store_be16(udp + 2, 9999);
  pkt::store_be16(udp + 4, static_cast<std::uint16_t>(len - 34));
  pkt::store_be16(udp + 6, 0);  // no UDP checksum (allowed for IPv4)
  pkt::store_be32(f.data() + kPayloadAt, kMagic);
  return f;
}

int run(std::span<char*> args) {
  Options o;
  ArgReader reader(args);
  while (const auto next = reader.next()) {
    const std::string_view a = *next;
    if (a == "--iface")
      o.iface = reader.value(a);
    else if (a == "--dst-mac")
      o.dst = *MacAddress::parse(reader.value(a));
    else if (a == "--src-mac")
      o.src = *MacAddress::parse(reader.value(a));
    else if (a == "--size")
      o.size = std::stoul(reader.value(a));
    else if (a == "--rate")
      o.rate = std::stoull(reader.value(a));
    else if (a == "--duration")
      o.duration = std::stod(reader.value(a));
    else if (a == "--random-src")
      o.random_src = static_cast<std::uint32_t>(std::stoul(reader.value(a)));
    else if (a == "--cpu")
      o.cpu = std::stoi(reader.value(a));
    else {
      std::cerr
          << "usage: ss-pktgen --iface IF --dst-mac MAC [--size N] [--rate PPS] [--duration S]"
             " [--random-src N] [--cpu N]\n";
      return 2;
    }
  }
  if (o.iface.empty() || o.size < 64 || o.size > 1518) {
    std::cerr << "ss-pktgen: need --iface and a --size of 64..1518\n";
    return 2;
  }
  if (o.cpu >= 0) {
    (void)sys::pin_current_thread(o.cpu);
  }
  dp::AfPacketOptions po;
  po.ifname = o.iface;
  po.tx_frames = 2048;
  po.rx_frames = 64;
  dp::AfPacketPort port(po);

  auto frame = build_template(o);
  const auto start = sys::monotonic_ns();
  const auto end = start + static_cast<std::uint64_t>(o.duration * 1e9);
  const std::uint64_t interval_ns = o.rate == 0 ? 0 : 1'000'000'000ULL / o.rate;
  std::uint64_t seq = 0;
  std::uint64_t full = 0;
  std::uint64_t next_due = start;
  for (;;) {
    const auto now = sys::monotonic_ns();
    if (now >= end) {
      break;
    }
    if (interval_ns != 0 && now < next_due) {
      continue;  // busy-wait pacing: sleeping is far too coarse at these rates
    }
    // Up to 32 frames per kick; with pacing, as many as are due.
    std::size_t burst = 32;
    if (interval_ns != 0) {
      burst = std::min<std::uint64_t>(32, (now - next_due) / interval_ns + 1);
    }
    for (std::size_t i = 0; i < burst; ++i) {
      pkt::store_be32(frame.data() + kPayloadAt + 4, static_cast<std::uint32_t>(seq));
      // Low 32 bits of the clock: latency is computed modulo 2^32 ns (exact below 4.29 s).
      pkt::store_be32(frame.data() + kPayloadAt + 8,
                      static_cast<std::uint32_t>(sys::monotonic_ns()));
      if (o.random_src != 0) {
        MacAddress::from_u64(0x020000000000ULL | (0xC0000000ULL + (seq % o.random_src)))
            .write_to(frame.data() + 6);
      }
      if (!port.tx(frame, std::nullopt)) {
        ++full;
        port.tx_flush();
        break;  // ring full: let the kernel drain, retry this sequence number
      }
      ++seq;
    }
    port.tx_flush();
    next_due += interval_ns * burst;
  }
  const double secs = static_cast<double>(sys::monotonic_ns() - start) / 1e9;
  std::cout << "{\"sent\":" << seq << ",\"seconds\":" << secs
            << ",\"pps\":" << static_cast<std::uint64_t>(static_cast<double>(seq) / secs)
            << ",\"size\":" << o.size << ",\"ring_full\":" << full << "}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::cerr << "ss-pktgen: " << e.what() << "\n";
    return 1;
  }
}
