#pragma once

/// @file
/// Creating and attaching the shared region over a memfd.

#include "softswitch/shm/region.hpp"
#include "softswitch/sys/shared_memory.hpp"

namespace softswitch::shm {

/// Control plane: creates a memfd large enough for a SharedRegion and constructs it in place.
[[nodiscard]] sys::SharedMemory create_region_memory(const std::string& name);
[[nodiscard]] SharedRegion& construct_region(const sys::SharedMemory& memory);

/// Data plane: validates magic, version and size of a received region.
/// Throws std::runtime_error on mismatch (e.g. binaries from different builds).
[[nodiscard]] SharedRegion& attach_region(const sys::SharedMemory& memory);

}  // namespace softswitch::shm
