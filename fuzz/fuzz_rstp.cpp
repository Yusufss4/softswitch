// Feeds one RSTP bridge arbitrary BPDUs, link flaps and timer ticks, and checks the invariants
// that must hold after every event:
//  - at most one root port, and it is the only port with the root role,
//  - the root port forwards; alternate, backup and disabled ports discard,
//  - a bridge with no root port believes it is the root, at cost 0.
#include <fuzzer/FuzzedDataProvider.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "softswitch/stp/rstp.hpp"

using namespace softswitch;
using namespace softswitch::stp;

namespace {

void check(const Rstp& rstp) {
  std::size_t roots = 0;
  for (std::size_t i = 0; i < rstp.num_ports(); ++i) {
    const auto p = static_cast<PortId>(i);
    const auto& st = rstp.status(p);
    switch (st.role) {
      case Role::kRoot:
        ++roots;
        if (rstp.root_port() != p || st.state != State::kForwarding) std::abort();
        break;
      case Role::kAlternate:
      case Role::kBackup:
      case Role::kDisabled:
        if (st.state != State::kDiscarding) std::abort();
        break;
      case Role::kDesignated:
        break;
    }
    if (!st.enabled && st.role != Role::kDisabled) std::abort();
  }
  if (roots > 1) std::abort();
  if (!rstp.root_port() && (!rstp.is_root_bridge() || rstp.root_cost() != 0)) std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  FuzzedDataProvider in(data, size);
  constexpr std::size_t kPorts = 4;
  BridgeConfig cfg;
  cfg.priority = static_cast<std::uint16_t>(in.ConsumeIntegralInRange<int>(0, 15) * 4096);
  cfg.mac = MacAddress::from_u64(0x020000000000ULL | in.ConsumeIntegral<std::uint16_t>());
  Rstp rstp(cfg, kPorts, {});
  auto now = Clock::time_point{};
  for (PortId p = 0; p < kPorts; ++p) {
    rstp.set_port_enabled(p, true, now);
  }
  while (in.remaining_bytes() > 0) {
    const auto port = static_cast<PortId>(in.ConsumeIntegralInRange<int>(0, kPorts - 1));
    switch (in.ConsumeIntegralInRange<int>(0, 3)) {
      case 0: {
        pkt::Bpdu b;
        b.type = in.PickValueInArray({pkt::kBpduTypeRst, pkt::kBpduTypeConfig, pkt::kBpduTypeTcn});
        b.flags = in.ConsumeIntegral<std::uint8_t>();
        // Small identifier spaces so that ties, loops and same-sender updates actually happen.
        b.root_id = (std::uint64_t{in.ConsumeIntegralInRange<std::uint16_t>(0, 3)} << 60U) |
                    in.ConsumeIntegralInRange<std::uint64_t>(0, 7);
        b.root_path_cost = in.ConsumeIntegralInRange<std::uint32_t>(0, 100000);
        b.bridge_id = (std::uint64_t{in.ConsumeIntegralInRange<std::uint16_t>(0, 3)} << 60U) |
                      in.ConsumeIntegralInRange<std::uint64_t>(0, 7);
        b.port_id = in.ConsumeIntegral<std::uint16_t>();
        b.message_age = in.ConsumeIntegralInRange<std::uint16_t>(0, 30 * 256);
        b.max_age = 20 * 256;
        rstp.receive(port, b, now);
        break;
      }
      case 1:
        rstp.set_port_enabled(port, in.ConsumeBool(), now);
        break;
      case 2:
        now += std::chrono::milliseconds{in.ConsumeIntegralInRange<int>(0, 5000)};
        rstp.tick(now);
        break;
      default:
        rstp.set_bridge_priority(
            static_cast<std::uint16_t>(in.ConsumeIntegralInRange<int>(0, 15) * 4096), now);
        break;
    }
    check(rstp);
  }
  return 0;
}
