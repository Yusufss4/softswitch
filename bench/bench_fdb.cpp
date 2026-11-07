#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "softswitch/l2/fdb.hpp"
#include "softswitch/shm/tables.hpp"

namespace {

using softswitch::MacAddress;
using softswitch::PortId;
using softswitch::l2::Fdb;

std::vector<MacAddress> macs(std::size_t n) {
  std::vector<MacAddress> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(MacAddress::from_u64(0x020000000000ULL + i));
  }
  return out;
}

// Control-plane cost of absorbing a learn event (new MAC each iteration, then refreshes).
void BM_FdbLearn(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto addrs = macs(n);
  Fdb fdb(n);
  const auto now = Fdb::Clock::now();
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(fdb.learn(addrs[i], 10, static_cast<PortId>(i % 8), now));
    i = (i + 1 == n) ? 0 : i + 1;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FdbLearn)->Arg(4096)->Arg(8192);

// Publishing cost: rebuilding the shared-memory FDB from the control-plane table.
void BM_FdbPublishRebuild(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto addrs = macs(n);
  Fdb fdb(n);
  const auto now = Fdb::Clock::now();
  for (std::size_t i = 0; i < n; ++i) {
    (void)fdb.learn(addrs[i], 10, static_cast<PortId>(i % 8), now);
  }
  auto tables = std::make_unique<softswitch::shm::Tables>();
  for (auto _ : state) {
    softswitch::shm::clear_dynamic(*tables);
    for (const auto& [key, e] : fdb.entries()) {
      (void)softswitch::shm::fdb_insert(*tables, e.mac, e.vlan, e.port, e.is_static);
    }
    benchmark::DoNotOptimize(tables->fdb_count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_FdbPublishRebuild)->Arg(64)->Arg(1024)->Arg(8192);

}  // namespace
