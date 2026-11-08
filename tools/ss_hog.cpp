// ss-hog: a periodic real-time CPU hog, the "medium-priority task" of a priority inversion.
//
//   ss-hog --cpu N [--priority 30] [--busy-ms 20] [--period-ms 50] [--duration S]
//
// Pinned to one CPU at SCHED_FIFO --priority, it spins for --busy-ms out of every --period-ms.
// While it spins, nothing of lower priority runs on that CPU: in particular a SCHED_OTHER
// control plane holding a lock the real-time data plane wants. The duty cycle stays well
// under the kernel's RT throttling budget, so the CPU is never monopolised.
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <thread>

#include "softswitch/common/args.hpp"
#include "softswitch/sys/realtime.hpp"

namespace {

using namespace softswitch;

int run(std::span<char*> args) {
  int cpu = -1;
  int priority = 30;
  std::uint64_t busy_ms = 20;
  std::uint64_t period_ms = 50;
  double duration = 10.0;
  ArgReader reader(args);
  while (const auto next = reader.next()) {
    const std::string_view a = *next;
    if (a == "--cpu") {
      cpu = std::stoi(reader.value(a));
    } else if (a == "--priority") {
      priority = std::stoi(reader.value(a));
    } else if (a == "--busy-ms") {
      busy_ms = std::stoull(reader.value(a));
    } else if (a == "--period-ms") {
      period_ms = std::stoull(reader.value(a));
    } else if (a == "--duration") {
      duration = std::stod(reader.value(a));
    } else {
      std::cerr
          << "usage: ss-hog --cpu N [--priority P] [--busy-ms B] [--period-ms T] [--duration S]\n";
      return 2;
    }
  }
  if (cpu < 0 || busy_ms == 0 || busy_ms * 10 > period_ms * 8) {
    std::cerr << "ss-hog: need --cpu, and --busy-ms at most 80 % of --period-ms\n";
    return 2;
  }
  for (const std::string& error :
       {sys::pin_current_thread(cpu), sys::set_current_thread_fifo(priority)}) {
    if (!error.empty()) {
      std::cerr << "ss-hog: " << error << "\n";
      return 1;
    }
  }
  const std::uint64_t busy_ns = busy_ms * 1'000'000;
  const std::uint64_t period_ns = period_ms * 1'000'000;
  const std::uint64_t end = sys::monotonic_ns() + static_cast<std::uint64_t>(duration * 1e9);
  std::uint64_t periods = 0;
  for (std::uint64_t start = sys::monotonic_ns(); start < end; start += period_ns) {
    while (sys::monotonic_ns() < start + busy_ns) {
      // spin: this is the point
    }
    ++periods;
    const std::uint64_t now = sys::monotonic_ns();
    if (now < start + period_ns) {
      std::this_thread::sleep_for(std::chrono::nanoseconds{start + period_ns - now});
    }
  }
  std::cout << "{\"periods\":" << periods << ",\"busy_ms\":" << busy_ms
            << ",\"period_ms\":" << period_ms << ",\"priority\":" << priority << ",\"cpu\":" << cpu
            << "}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::cerr << "ss-hog: " << e.what() << "\n";
    return 1;
  }
}
