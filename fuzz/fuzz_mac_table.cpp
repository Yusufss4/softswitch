// Drives MacTable with an arbitrary sequence of learn / lookup / age operations and checks
// invariants that must hold for any input.
#include <fuzzer/FuzzedDataProvider.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "softswitch/mac_table.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  FuzzedDataProvider fdp(data, size);
  using softswitch::MacTable;
  const auto aging = std::chrono::seconds{fdp.ConsumeIntegralInRange<int>(1, 600)};
  MacTable table{aging};
  auto now = MacTable::Clock::time_point{};

  while (fdp.remaining_bytes() > 0) {
    softswitch::MacAddress mac{};
    for (auto& b : mac) b = fdp.ConsumeIntegral<std::uint8_t>();
    switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
      case 0: {
        const auto port = fdp.ConsumeIntegral<softswitch::PortId>();
        table.learn(mac, port, now);
        // A freshly learned entry must be found on the port it was learned on.
        if (table.lookup(mac, now) != port) std::abort();
        break;
      }
      case 1:
        (void)table.lookup(mac, now);
        break;
      case 2:
        now += std::chrono::seconds{fdp.ConsumeIntegralInRange<int>(0, 1000)};
        break;
      default: {
        const auto before = table.size();
        const auto removed = table.age_out(now);
        if (removed > before || table.size() != before - removed) std::abort();
        break;
      }
    }
  }
  return 0;
}
