#include "softswitch/shm/region_mapping.hpp"

#include <new>
#include <stdexcept>
#include <string>

namespace softswitch::shm {

sys::SharedMemory create_region_memory(const std::string& name) {
  return sys::SharedMemory::create(name, sizeof(SharedRegion));
}

SharedRegion& construct_region(const sys::SharedMemory& memory) {
  if (memory.size() < sizeof(SharedRegion)) {
    throw std::runtime_error("shared memory too small for SharedRegion");
  }
  return *::new (memory.data()) SharedRegion{};
}

SharedRegion& attach_region(const sys::SharedMemory& memory) {
  if (memory.size() < sizeof(SharedRegion)) {
    throw std::runtime_error("shared region is smaller than expected (" +
                             std::to_string(memory.size()) + " < " +
                             std::to_string(sizeof(SharedRegion)) + " bytes)");
  }
  auto* region = std::launder(static_cast<SharedRegion*>(memory.data()));
  if (region->magic != kRegionMagic || region->version != kRegionVersion ||
      region->size != sizeof(SharedRegion)) {
    throw std::runtime_error(
        "shared region layout mismatch: control and data plane binaries differ");
  }
  return *region;
}

}  // namespace softswitch::shm
