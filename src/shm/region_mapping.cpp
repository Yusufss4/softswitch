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

void set_publish_mode(SharedRegion& region, PublishMode mode) {
  region.publish_mode = mode;
  if (mode == PublishMode::kQsbr) {
    return;
  }
  pthread_mutexattr_t attr;
  if (::pthread_mutexattr_init(&attr) != 0) {
    throw std::runtime_error("pthread_mutexattr_init failed");
  }
  int rc = ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  if (rc == 0 && mode == PublishMode::kMutexPi) {
    rc = ::pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
  }
  if (rc == 0) {
    rc = ::pthread_mutex_init(&region.table_lock, &attr);
  }
  ::pthread_mutexattr_destroy(&attr);
  if (rc != 0) {
    throw std::runtime_error("cannot initialise the shared table lock (error " +
                             std::to_string(rc) + ")");
  }
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
