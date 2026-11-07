#pragma once

/// @file
/// Real-time thread setup for the data plane: CPU affinity, SCHED_FIFO and locked memory.

#include <cstdint>
#include <string>

namespace softswitch::sys {

/// Each returns an empty string on success, otherwise a human-readable error.
[[nodiscard]] std::string pin_current_thread(int cpu);
[[nodiscard]] std::string set_current_thread_fifo(int priority);
[[nodiscard]] std::string lock_all_memory();
/// Sets the thread name shown by top/ps/perf (max 15 chars).
void name_current_thread(const std::string& name);

/// Clock readings in nanoseconds. AF_PACKET ring timestamps use CLOCK_REALTIME.
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;
[[nodiscard]] std::uint64_t realtime_ns() noexcept;

}  // namespace softswitch::sys
