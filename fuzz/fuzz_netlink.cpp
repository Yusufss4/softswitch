// Feeds arbitrary datagrams to the rtnetlink link-message parser (kernel input, but parsed
// defensively): it must never read out of bounds or crash.
#include <cstdint>
#include <span>

#include "softswitch/cp/netlink.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto events =
      softswitch::cp::parse_link_messages(std::span<const std::uint8_t>(data, size));
  for (const auto& ev : events) {
    (void)ev.oper_up();
    (void)ev.name.size();
  }
  return 0;
}
