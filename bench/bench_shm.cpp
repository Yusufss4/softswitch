#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

#include "softswitch/shm/region.hpp"
#include "softswitch/shm/spsc_ring.hpp"
#include "softswitch/shm/tables.hpp"

namespace {

using namespace softswitch;
using namespace softswitch::shm;

void BM_SpscPushPop(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<LearnEvent, 4096>>();
  LearnEvent ev{};
  for (auto _ : state) {
    benchmark::DoNotOptimize(ring->try_push(ev));
    benchmark::DoNotOptimize(ring->try_pop(ev));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);

std::unique_ptr<Tables> filled_tables(std::size_t entries) {
  auto t = std::make_unique<Tables>();
  for (std::size_t i = 0; i < entries; ++i) {
    (void)fdb_insert(*t, MacAddress::from_u64(0x020000000000ULL + i), 10,
                     static_cast<PortId>(i % 8), false);
  }
  return t;
}

void BM_FdbLookupHit(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto t = filled_tables(n);
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(fdb_lookup(*t, MacAddress::from_u64(0x020000000000ULL + i), 10));
    i = (i + 1 == n) ? 0 : i + 1;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FdbLookupHit)->Arg(64)->Arg(4096)->Arg(kFdbSlots / 2);

void BM_FdbLookupMiss(benchmark::State& state) {
  const auto t = filled_tables(static_cast<std::size_t>(state.range(0)));
  std::uint64_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(fdb_lookup(*t, MacAddress::from_u64(0x0A0000000000ULL + i++), 10));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FdbLookupMiss)->Arg(64)->Arg(kFdbSlots / 2);

void BM_RouteLookup(benchmark::State& state) {
  auto t = std::make_unique<Tables>();
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(state.range(0)); ++i) {
    (void)route_insert(*t, {Ipv4Prefix{Ipv4Address{(10U << 24U) | (i << 8U)}, 24}, 0, 10, {}});
  }
  const Ipv4Address dst{(10U << 24U) | 5U};  // matches the first-inserted /24
  for (auto _ : state) {
    benchmark::DoNotOptimize(route_lookup(*t, dst));
  }
}
BENCHMARK(BM_RouteLookup)->Arg(4)->Arg(64);

}  // namespace
