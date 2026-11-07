#pragma once

/// @file
/// Attach handshake between ss-dataplane and softswitchd over a SOCK_SEQPACKET Unix socket.
///
///   DP -> CP   DataPlaneHello
///   CP -> DP   ControlPlaneWelcome + SCM_RIGHTS [memfd, notify eventfd, kick eventfd]
///
/// After that the socket stays open only as a liveness signal: when either side exits, the
/// other sees EOF. "notify" is signalled by the DP when the learn/punt rings get new entries;
/// "kick" by the CP after it pushes to the inject ring.

#include <cstdint>

#include "softswitch/shm/region.hpp"

namespace softswitch::shm {

struct DataPlaneHello {
  std::uint64_t magic = kRegionMagic;
  std::uint32_t version = kRegionVersion;
  std::int32_t pid = 0;
};

struct ControlPlaneWelcome {
  std::uint64_t magic = kRegionMagic;
  std::uint32_t version = kRegionVersion;
  std::uint32_t region_size = sizeof(SharedRegion);
};

inline constexpr std::size_t kWelcomeFdMemory = 0;
inline constexpr std::size_t kWelcomeFdNotify = 1;
inline constexpr std::size_t kWelcomeFdKick = 2;
inline constexpr std::size_t kWelcomeFdCount = 3;

}  // namespace softswitch::shm
