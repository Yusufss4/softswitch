// softswitchd: the control plane. Owns configuration and protocol state and publishes
// forwarding tables to ss-dataplane through shared memory.
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include "softswitch/common/args.hpp"
#include "softswitch/common/log.hpp"
#include "softswitch/cp/control_plane.hpp"
#include "softswitch/cp/event_loop.hpp"
#include "softswitch/version.hpp"

namespace {

void usage() {
  std::cout << "usage: softswitchd --name NAME --ports IF1,IF2,... [--config FILE] [--run-dir DIR] "
               "[--debug]\n";
}

std::vector<std::string> split_csv(const std::string& text) {
  std::vector<std::string> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

int run(std::span<char*> args) {
  softswitch::cp::ControlPlaneOptions options;
  softswitch::ArgReader reader(args);
  while (const auto next = reader.next()) {
    const std::string_view arg = *next;
    auto value = [&]() { return reader.value(arg); };
    if (arg == "--name") {
      options.name = value();
    } else if (arg == "--ports") {
      options.ports = split_csv(value());
    } else if (arg == "--run-dir") {
      options.run_dir = value();
    } else if (arg == "--config") {
      options.config_file = value();
    } else if (arg == "--http-port") {
      options.http_port = static_cast<std::uint16_t>(std::stoul(value()));
    } else if (arg == "--http-bind") {
      options.http_bind = value();
    } else if (arg == "--debug") {
      softswitch::log::set_level(softswitch::log::Level::kDebug);
    } else if (arg == "--version") {
      std::cout << "softswitchd " << softswitch::kVersion << '\n';
      return 0;
    } else {
      usage();
      return arg == "--help" ? 0 : 2;
    }
  }
  softswitch::log::set_tag(options.name + "/cp");
  softswitch::cp::EventLoop loop;
  loop.stop_on_signals();
  softswitch::cp::ControlPlane cp(loop, options);
  cp.start();
  loop.run();
  softswitch::log::info("shutting down");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    softswitch::log::error(e.what());
    return 1;
  }
}
