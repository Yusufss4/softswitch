#include <algorithm>
#include <iostream>
#include <span>
#include <string_view>

#include "softswitch/mac_table.hpp"
#include "softswitch/version.hpp"

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  const bool want_version = std::ranges::any_of(args.subspan(1), [](const char* arg) {
    const std::string_view sv{arg};
    return sv == "--version" || sv == "-V";
  });
  if (want_version) {
    std::cout << "softswitch " << softswitch::kVersion << '\n';
    return 0;
  }

  const softswitch::MacTable fdb;
  std::cout << "softswitch " << softswitch::kVersion << " - L2/L3 software switch\n"
            << "FDB entries: " << fdb.size() << '\n';
  return 0;
}
