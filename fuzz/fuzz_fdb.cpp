// Drives the control-plane FDB with arbitrary learn/static/touch/age/flush sequences and checks
// its invariants: lookups agree with the last learn, statics never move or age, capacity holds.
#include <fuzzer/FuzzedDataProvider.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

#include "softswitch/l2/fdb.hpp"

using namespace softswitch;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  FuzzedDataProvider in(data, size);
  l2::Fdb fdb(in.ConsumeIntegralInRange<std::size_t>(1, 64));
  auto now = l2::Fdb::Clock::time_point{};
  while (in.remaining_bytes() > 0) {
    const auto mac =
        MacAddress::from_u64(0x020000000000ULL | in.ConsumeIntegralInRange<std::uint64_t>(0, 31));
    const auto vlan = static_cast<VlanId>(in.ConsumeIntegralInRange<int>(1, 3));
    const auto port = static_cast<PortId>(in.ConsumeIntegralInRange<int>(0, 3));
    switch (in.ConsumeIntegralInRange<int>(0, 5)) {
      case 0: {
        const auto* before = fdb.find(mac, vlan);
        const bool was_static = before != nullptr && before->is_static;
        const PortId static_port = was_static ? before->port : kInvalidPort;
        const auto r = fdb.learn(mac, vlan, port, now);
        const auto after = fdb.lookup(mac, vlan);
        if (was_static && after != static_port) std::abort();
        if ((r == l2::LearnResult::kNew || r == l2::LearnResult::kMoved) && after != port)
          std::abort();
        break;
      }
      case 1:
        (void)fdb.add_static(mac, vlan, port);
        break;
      case 2:
        fdb.touch(mac, vlan, now);
        break;
      case 3:
        now += std::chrono::seconds{in.ConsumeIntegralInRange<int>(0, 400)};
        break;
      case 4: {
        const auto* e = fdb.find(mac, vlan);
        const bool was_static = e != nullptr && e->is_static;
        (void)fdb.age_out(now, std::chrono::seconds{300});
        if (was_static && fdb.find(mac, vlan) == nullptr) std::abort();
        break;
      }
      default:
        (void)fdb.flush(in.ConsumeBool() ? std::optional<PortId>{port} : std::nullopt,
                        in.ConsumeBool() ? std::optional<VlanId>{vlan} : std::nullopt);
        break;
    }
    if (fdb.size() > fdb.capacity()) std::abort();
  }
  return 0;
}
