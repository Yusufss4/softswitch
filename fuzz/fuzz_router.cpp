// Feeds arbitrary frames to every l3::Router entry point (they come from hosts via the punt
// ring) interleaved with timer ticks. Nothing may crash, and every frame the router emits must
// be a well-formed Ethernet frame that fits in a packet message.
#include <fuzzer/FuzzedDataProvider.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "softswitch/l3/router.hpp"
#include "softswitch/pkt/ethernet.hpp"

using namespace softswitch;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  FuzzedDataProvider in(data, size);
  l3::RouterConfig cfg;
  cfg.mac = *MacAddress::parse("02:00:00:00:00:fe");
  cfg.svis = {{10, *Ipv4Prefix::parse("10.0.10.1/24")}, {20, *Ipv4Prefix::parse("10.0.20.1/24")}};
  cfg.static_routes = {{*Ipv4Prefix::parse("192.168.0.0/16"), *Ipv4Address::parse("10.0.20.9")}};
  l3::Router router(cfg);
  auto now = l3::Clock::time_point{};
  while (in.remaining_bytes() > 0) {
    const auto op = in.ConsumeIntegralInRange<int>(0, 4);
    const auto vlan = static_cast<VlanId>(in.PickValueInArray({10, 20, 30}));
    const auto bytes =
        in.ConsumeBytes<std::uint8_t>(in.ConsumeIntegralInRange<std::size_t>(0, 200));
    std::vector<l3::Output> out;
    switch (op) {
      case 0:
        out = router.on_arp(vlan, bytes, now);
        break;
      case 1:
        out = router.on_to_router(vlan, bytes);
        break;
      case 2:
        out = router.on_ttl_expired(vlan, bytes);
        break;
      case 3:
        out = router.on_neighbor_miss(vlan, bytes, now);
        break;
      default:
        now += std::chrono::milliseconds{in.ConsumeIntegralInRange<int>(0, 400000)};
        out = router.tick(now);
        break;
    }
    for (const auto& o : out) {
      if (o.frame.size() > 1536 || !pkt::parse_ethernet(o.frame)) std::abort();
    }
  }
  return 0;
}
