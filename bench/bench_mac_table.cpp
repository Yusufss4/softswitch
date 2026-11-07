#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "softswitch/mac_table.hpp"

namespace {

using softswitch::MacAddress;
using softswitch::MacTable;
using softswitch::PortId;

std::vector<MacAddress> make_macs(std::size_t n) {
  std::vector<MacAddress> macs(n);
  for (std::size_t i = 0; i < n; ++i) {
    macs[i] = MacAddress{0x02,
                         0x00,
                         static_cast<std::uint8_t>(i >> 24U),
                         static_cast<std::uint8_t>(i >> 16U),
                         static_cast<std::uint8_t>(i >> 8U),
                         static_cast<std::uint8_t>(i)};
  }
  return macs;
}

void BM_MacTableLookupHit(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto macs = make_macs(n);
  const auto now = MacTable::Clock::now();
  MacTable table;
  for (std::size_t i = 0; i < n; ++i) {
    table.learn(macs[i], static_cast<PortId>(i % 8), now);
  }
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.lookup(macs[i], now));
    i = (i + 1 == n) ? 0 : i + 1;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MacTableLookupHit)->Arg(64)->Arg(4096)->Arg(65536);

void BM_MacTableLearn(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto macs = make_macs(n);
  const auto now = MacTable::Clock::now();
  MacTable table;
  std::size_t i = 0;
  for (auto _ : state) {
    table.learn(macs[i], static_cast<PortId>(i % 8), now);
    i = (i + 1 == n) ? 0 : i + 1;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MacTableLearn)->Arg(4096)->Arg(65536);

}  // namespace
