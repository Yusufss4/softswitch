#include "softswitch/sys/realtime.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace softswitch::sys {
namespace {
std::string error_string(int err) { return std::strerror(err); }

std::uint64_t clock_ns(clockid_t clock) noexcept {
  timespec ts{};
  ::clock_gettime(clock, &ts);
  return (static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL) +
         static_cast<std::uint64_t>(ts.tv_nsec);
}
}  // namespace

std::string pin_current_thread(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
  return rc == 0 ? std::string{} : "pthread_setaffinity_np: " + error_string(rc);
}

std::string set_current_thread_fifo(int priority) {
  sched_param param{};
  param.sched_priority = priority;
  const int rc = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
  return rc == 0 ? std::string{}
                 : "pthread_setschedparam(SCHED_FIFO): " + error_string(rc) +
                       " (needs CAP_SYS_NICE or root)";
}

std::string lock_all_memory() {
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    return "mlockall: " + error_string(errno) + " (needs CAP_IPC_LOCK or a higher RLIMIT_MEMLOCK)";
  }
  return {};
}

void name_current_thread(const std::string& name) {
  (void)::pthread_setname_np(::pthread_self(), name.substr(0, 15).c_str());
}

std::uint64_t monotonic_ns() noexcept { return clock_ns(CLOCK_MONOTONIC); }
std::uint64_t realtime_ns() noexcept { return clock_ns(CLOCK_REALTIME); }

}  // namespace softswitch::sys
