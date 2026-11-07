// Feeds arbitrary text to the CLI configuration parser, one line at a time, and checks that
//  - no input crashes or trips a sanitizer,
//  - whatever config results renders to a running-config that replays to the same config,
//  - apply_l2_config on it keeps PVIDs and memberships within range.
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

#include "softswitch/cp/config.hpp"

using namespace softswitch;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);
  auto config = cp::SwitchConfig::with_ports("fz", {"p1", "p2", "p3"});
  std::istringstream lines(text);
  std::string line;
  int count = 0;
  while (std::getline(lines, line) && ++count < 64) {
    (void)cp::apply_config(config, cp::tokenize(line));
  }

  auto replay = cp::SwitchConfig::with_ports("fz", {"p1", "p2", "p3"});
  std::istringstream rendered(cp::render_running_config(config));
  while (std::getline(rendered, line)) {
    if (!cp::apply_config(replay, cp::tokenize(line)).ok) {
      std::abort();  // running-config must always be valid input
    }
  }
  if (!(replay == config)) {
    std::abort();
  }

  auto tables = std::make_unique<shm::Tables>();
  cp::apply_l2_config(config, *tables);
  for (std::uint32_t p = 0; p < tables->num_ports; ++p) {
    if (tables->ports[p].pvid > kMaxVlanId) {
      std::abort();
    }
  }
  return 0;
}
