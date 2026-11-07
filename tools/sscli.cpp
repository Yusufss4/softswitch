// sscli: command-line client for softswitchd's CLI socket.
//
//   sscli [-s NAME] [--run-dir DIR] [COMMAND...]
// With a command, runs it once and prints the output. Without one, starts an interactive
// session reading commands from stdin.
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "softswitch/sys/fd.hpp"
#include "softswitch/sys/unix_socket.hpp"

namespace {

using softswitch::sys::UniqueFd;

/// Sends one line and reads the reply up to the NUL terminator. Returns false on EOF.
bool roundtrip(int fd, const std::string& line, std::string& reply) {
  const std::string msg = line + "\n";
  if (::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size())) {
    return false;
  }
  reply.clear();
  std::array<char, 4096> buf{};
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      return false;
    }
    const std::string_view chunk(buf.data(), static_cast<std::size_t>(n));
    const auto nul = chunk.find('\0');
    reply.append(chunk.substr(0, nul));
    if (nul != std::string_view::npos) {
      return true;
    }
  }
}

int run(std::span<char*> args) {
  std::string name = "sw1";
  std::string run_dir = "/tmp/softswitch";
  std::string command;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if ((arg == "-s" || arg == "--switch") && i + 1 < args.size()) {
      name = args[++i];
    } else if (arg == "--run-dir" && i + 1 < args.size()) {
      run_dir = args[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: sscli [-s NAME] [--run-dir DIR] [COMMAND...]\n";
      return 0;
    } else {
      command += (command.empty() ? "" : " ") + std::string(arg);
    }
  }
  const std::string path = run_dir + "/" + name + ".cli.sock";
  const UniqueFd fd = softswitch::sys::unix_connect(path, SOCK_STREAM);
  std::string reply;
  if (!command.empty()) {
    if (!roundtrip(fd.get(), command, reply)) {
      std::cerr << "sscli: connection closed\n";
      return 1;
    }
    std::cout << reply;
    return reply.rfind("% ", 0) == 0 ? 1 : 0;
  }
  const bool tty = ::isatty(STDIN_FILENO) != 0;
  std::string line;
  for (;;) {
    if (tty) {
      std::cout << name << "# " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      break;
    }
    if (line == "exit" || line == "quit") {
      break;
    }
    if (!roundtrip(fd.get(), line, reply)) {
      std::cerr << "sscli: connection closed\n";
      return 1;
    }
    std::cout << reply << std::flush;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::cerr << "sscli: " << e.what() << "\n";
    return 1;
  }
}
