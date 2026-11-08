// ss-sink: counts ss-pktgen frames and measures one-way latency.
//
//   ss-sink --iface IF [--duration S] [--cpu N]
//
// Latency = CLOCK_MONOTONIC at receive minus the timestamp ss-pktgen wrote, so it covers the
// sender's TX ring, both veth hops and the switch. Every sample is kept (they are 4 bytes), so
// percentiles are exact. Prints one JSON line, including a log-spaced latency histogram
// ("hist": [[upper_us, count], ...], 8 buckets per power of two).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "softswitch/common/args.hpp"
#include "softswitch/dp/af_packet_port.hpp"
#include "softswitch/pkt/bytes.hpp"
#include "softswitch/sys/realtime.hpp"

namespace {

using namespace softswitch;

constexpr std::uint32_t kMagic = 0x53535747;
constexpr std::size_t kPayloadAt = 14 + 20 + 8;

double percentile(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto n = static_cast<double>(sorted.size());
  const auto idx = static_cast<std::size_t>(std::clamp(std::ceil(p * n) - 1.0, 0.0, n - 1.0));
  return sorted[idx] / 1000.0;  // ns -> us
}

int run(std::span<char*> args) {
  std::string iface;
  double duration = 6.0;
  int cpu = -1;
  ArgReader reader(args);
  while (const auto next = reader.next()) {
    const std::string_view a = *next;
    if (a == "--iface")
      iface = reader.value(a);
    else if (a == "--duration")
      duration = std::stod(reader.value(a));
    else if (a == "--cpu")
      cpu = std::stoi(reader.value(a));
    else {
      std::cerr << "usage: ss-sink --iface IF [--duration S] [--cpu N]\n";
      return 2;
    }
  }
  if (cpu >= 0) (void)sys::pin_current_thread(cpu);
  dp::AfPacketOptions po;
  po.ifname = iface;
  po.rx_frames = 8192;
  po.tx_frames = 64;
  dp::AfPacketPort port(po);

  std::vector<std::uint32_t> latency;
  latency.reserve(8'000'000);
  std::vector<dp::RxFrame> frames(256);
  std::uint64_t received = 0, max_seq = 0, first_ns = 0, last_ns = 0;
  const auto end = sys::monotonic_ns() + static_cast<std::uint64_t>(duration * 1e9);
  while (sys::monotonic_ns() < end) {
    const std::size_t n = port.rx_burst(frames);
    const auto now = sys::monotonic_ns();
    for (std::size_t i = 0; i < n; ++i) {
      const auto d = frames[i].data;
      if (d.size() < kPayloadAt + 12 || pkt::load_be16(d.data() + 36) != 9999 ||
          pkt::load_be32(d.data() + kPayloadAt) != kMagic) {
        continue;
      }
      const std::uint64_t seq = pkt::load_be32(d.data() + kPayloadAt + 4);
      const std::uint32_t sent = pkt::load_be32(d.data() + kPayloadAt + 8);
      ++received;
      max_seq = std::max(max_seq, seq);
      if (first_ns == 0) first_ns = now;
      last_ns = now;
      if (latency.size() < latency.capacity()) {
        latency.push_back(static_cast<std::uint32_t>(now) - sent);  // modulo 2^32 ns
      }
    }
    port.rx_release();
  }
  std::sort(latency.begin(), latency.end());
  const double secs = last_ns > first_ns ? static_cast<double>(last_ns - first_ns) / 1e9 : 0;
  double mean = 0;
  for (const auto v : latency) mean += v;
  mean = latency.empty() ? 0 : mean / static_cast<double>(latency.size()) / 1000.0;
  std::cout << "{\"received\":" << received << ",\"expected\":" << (received ? max_seq + 1 : 0)
            << ",\"sink_ring_drops\":" << port.rx_kernel_drops() << ",\"pps\":"
            << (secs > 0 ? static_cast<std::uint64_t>(static_cast<double>(received) / secs) : 0)
            << ",\"lat_us\":{\"min\":" << percentile(latency, 0.0) << ",\"mean\":" << mean
            << ",\"p50\":" << percentile(latency, 0.50) << ",\"p90\":" << percentile(latency, 0.90)
            << ",\"p99\":" << percentile(latency, 0.99)
            << ",\"p999\":" << percentile(latency, 0.999)
            << ",\"max\":" << (latency.empty() ? 0 : latency.back() / 1000.0) << "}";

  // Log-spaced buckets: 8 per power of two, from 1 us to ~16 s; empty buckets are left out.
  // Part of the JSON rather than a file, so the tool never opens a path it was given.
  std::cout << ",\"hist\":[";
  std::size_t idx = 0;
  bool first = true;
  for (int b = 0; b < 8 * 24; ++b) {
    const double upper_us = std::pow(2.0, b / 8.0);
    std::uint64_t count = 0;
    while (idx < latency.size() && latency[idx] / 1000.0 <= upper_us) {
      ++count;
      ++idx;
    }
    if (count != 0) {
      std::cout << (first ? "" : ",") << "[" << upper_us << "," << count << "]";
      first = false;
    }
  }
  std::cout << "]}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::cerr << "ss-sink: " << e.what() << "\n";
    return 1;
  }
}
