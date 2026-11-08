#pragma once

/// @file
/// Creating and attaching the shared region over a memfd.

#include "softswitch/shm/region.hpp"
#include "softswitch/sys/shared_memory.hpp"

namespace softswitch::shm {

/// Control plane: creates a memfd large enough for a SharedRegion and constructs it in place.
[[nodiscard]] sys::SharedMemory create_region_memory(const std::string& name);
[[nodiscard]] SharedRegion& construct_region(const sys::SharedMemory& memory);

/// Control plane, before the data plane attaches: selects how tables are published and, for
/// the lock modes, initialises the process-shared table lock (with priority inheritance for
/// PublishMode::kMutexPi). Throws on pthread errors.
void set_publish_mode(SharedRegion& region, PublishMode mode);

/// Data plane: validates magic, version and size of a received region.
/// Throws std::runtime_error on mismatch (e.g. binaries from different builds).
[[nodiscard]] SharedRegion& attach_region(const sys::SharedMemory& memory);

}  // namespace softswitch::shm
