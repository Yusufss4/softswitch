#include <iostream>

#include "softswitch/mac_table.hpp"

int main() {
  softswitch::MacTable fdb;
  std::cout << "softswitch 0.1.0 - L2/L3 software switch\n"
            << "FDB entries: " << fdb.size() << '\n';
  return 0;
}
