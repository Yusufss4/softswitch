#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <vector>

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace {

using namespace softswitch::pkt;

std::vector<std::uint8_t> ip_frame(std::size_t len) {
  std::vector<std::uint8_t> f(len, 0x5a);
  store_be16(f.data() + kMacAddrsLen, kEtherTypeIpv4);
  std::uint8_t* ip = f.data() + kEthHeaderLen;
  ip[0] = 0x45;
  store_be16(ip + 2, static_cast<std::uint16_t>(len - kEthHeaderLen));
  ip[8] = 64;
  store_be16(ip + 10, 0);
  store_be16(ip + 10, internet_checksum(std::span{f}.subspan(kEthHeaderLen, 20)));
  return f;
}

void BM_ParseEthernet(benchmark::State& state) {
  const auto f = ip_frame(64);
  for (auto _ : state) {
    benchmark::DoNotOptimize(parse_ethernet(f));
  }
}
BENCHMARK(BM_ParseEthernet);

void BM_InternetChecksum(benchmark::State& state) {
  const auto f = ip_frame(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(internet_checksum(f));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_InternetChecksum)->Arg(20)->Arg(1500);

void BM_DecrementTtlIncremental(benchmark::State& state) {
  auto f = ip_frame(64);
  auto l3 = std::span{f}.subspan(kEthHeaderLen);
  for (auto _ : state) {
    l3[8] = 64;
    benchmark::DoNotOptimize(decrement_ttl(l3));
  }
}
BENCHMARK(BM_DecrementTtlIncremental);

void BM_DecrementTtlFullRecompute(benchmark::State& state) {
  auto f = ip_frame(64);
  auto l3 = std::span{f}.subspan(kEthHeaderLen);
  for (auto _ : state) {
    l3[8] = 63;
    store_be16(l3.data() + 10, 0);
    store_be16(l3.data() + 10, internet_checksum(l3.first(20)));
    benchmark::DoNotOptimize(l3.data());
  }
}
BENCHMARK(BM_DecrementTtlFullRecompute);

void BM_CopyWithTag(benchmark::State& state) {
  const auto f = ip_frame(static_cast<std::size_t>(state.range(0)));
  std::vector<std::uint8_t> out(f.size() + kVlanTagLen);
  for (auto _ : state) {
    benchmark::DoNotOptimize(copy_with_tag(f, make_tci(10), out));
  }
}
BENCHMARK(BM_CopyWithTag)->Arg(64)->Arg(1500);

}  // namespace
