// ss-dataplane: attaches to softswitchd and forwards packets on a pinned SCHED_FIFO thread.
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "softswitch/common/log.hpp"
#include "softswitch/dp/dataplane.hpp"
#include "softswitch/version.hpp"

namespace {

void usage() {
  std::cout
      << "usage: ss-dataplane --socket PATH [--cpu N] [--priority N] [--no-rt]\n"
         "                    [--require-rt] [--tx-mode ring|sendto] [--batch N] [--name TAG]\n";
}

int run(std::span<char*> args) {
  softswitch::dp::DataPlaneOptions options;
  std::string tag = "dp";
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument(std::string(arg) + " needs a value");
      }
      return args[++i];
    };
    if (arg == "--socket") {
      options.control_socket = value();
    } else if (arg == "--cpu") {
      options.cpu = std::stoi(value());
    } else if (arg == "--priority") {
      options.priority = std::stoi(value());
    } else if (arg == "--no-rt") {
      options.priority = 0;
    } else if (arg == "--require-rt") {
      options.require_rt = true;
    } else if (arg == "--tx-mode") {
      const auto mode = value();
      options.tx_mode =
          mode == "sendto" ? softswitch::dp::TxMode::kSendto : softswitch::dp::TxMode::kRing;
    } else if (arg == "--batch") {
      options.batch = static_cast<std::size_t>(std::stoul(value()));
    } else if (arg == "--name") {
      tag = value() + "/dp";
    } else if (arg == "--version") {
      std::cout << "ss-dataplane " << softswitch::kVersion << '\n';
      return 0;
    } else {
      usage();
      return arg == "--help" ? 0 : 2;
    }
  }
  if (options.control_socket.empty()) {
    usage();
    return 2;
  }
  softswitch::log::set_tag(tag);
  softswitch::dp::DataPlane dataplane(options);
  dataplane.start();
  softswitch::log::info("attached to ", options.control_socket);
  dataplane.wait();
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
