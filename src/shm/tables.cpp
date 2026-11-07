#include "softswitch/shm/tables.hpp"

#include <algorithm>

namespace softswitch::shm {
namespace {

/// Generic linear probe: returns the slot holding a matching key, or the first free slot,
/// or nullptr if the table is completely full (impossible below the load-factor limit).
template <typename Slots, typename Match>
auto* probe(Slots& slots, std::uint64_t hash, const Match& match) noexcept {
  const std::size_t mask = slots.size() - 1;
  std::size_t index = static_cast<std::size_t>(hash) & mask;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    auto& slot = slots[index];
    if ((slot.flags & slot_flag::kUsed) == 0 || match(slot)) {
      return &slot;
    }
    index = (index + 1) & mask;
  }
  return static_cast<decltype(&slots[0])>(nullptr);
}

template <typename Slot>
const Slot* found(const Slot* slot) noexcept {
  return (slot != nullptr && (slot->flags & slot_flag::kUsed) != 0) ? slot : nullptr;
}

}  // namespace

const FdbSlot* fdb_lookup(const Tables& t, const MacAddress& mac, VlanId vlan) noexcept {
  return found(probe(t.fdb, fdb_hash(mac, vlan),
                     [&](const FdbSlot& s) { return s.mac == mac && s.vlan == vlan; }));
}

const MdbSlot* mdb_lookup(const Tables& t, Ipv4Address group, VlanId vlan) noexcept {
  return found(probe(t.mdb, mdb_hash(group, vlan),
                     [&](const MdbSlot& s) { return s.group == group && s.vlan == vlan; }));
}

const NeighborSlot* neighbor_lookup(const Tables& t, Ipv4Address ip) noexcept {
  return found(
      probe(t.neighbors, neighbor_hash(ip), [&](const NeighborSlot& s) { return s.ip == ip; }));
}

const RouteEntry* route_lookup(const Tables& t, Ipv4Address dst) noexcept {
  const std::size_t n = std::min<std::size_t>(t.num_routes, kMaxRoutes);
  for (std::size_t i = 0; i < n; ++i) {
    if (t.routes[i].prefix.contains(dst)) {
      return &t.routes[i];
    }
  }
  return nullptr;
}

void clear_dynamic(Tables& t) noexcept {
  t.fdb.fill(FdbSlot{});
  t.mdb.fill(MdbSlot{});
  t.neighbors.fill(NeighborSlot{});
  t.routes.fill(RouteEntry{});
  t.fdb_count = 0;
  t.mdb_count = 0;
  t.neighbor_count = 0;
  t.num_routes = 0;
}

bool fdb_insert(Tables& t, const MacAddress& mac, VlanId vlan, PortId port,
                bool is_static) noexcept {
  FdbSlot* slot = probe(t.fdb, fdb_hash(mac, vlan),
                        [&](const FdbSlot& s) { return s.mac == mac && s.vlan == vlan; });
  const bool is_new = slot != nullptr && (slot->flags & slot_flag::kUsed) == 0;
  if (slot == nullptr || (is_new && t.fdb_count >= kFdbSlots / 2)) {
    return false;
  }
  *slot =
      FdbSlot{mac, vlan, port,
              static_cast<std::uint8_t>(slot_flag::kUsed | (is_static ? slot_flag::kStatic : 0))};
  t.fdb_count += is_new ? 1U : 0U;
  return true;
}

bool mdb_insert(Tables& t, Ipv4Address group, VlanId vlan, PortMask ports) noexcept {
  MdbSlot* slot = probe(t.mdb, mdb_hash(group, vlan),
                        [&](const MdbSlot& s) { return s.group == group && s.vlan == vlan; });
  const bool is_new = slot != nullptr && (slot->flags & slot_flag::kUsed) == 0;
  if (slot == nullptr || (is_new && t.mdb_count >= kMdbSlots / 2)) {
    return false;
  }
  *slot = MdbSlot{group, vlan, slot_flag::kUsed, ports};
  t.mdb_count += is_new ? 1U : 0U;
  return true;
}

bool neighbor_insert(Tables& t, Ipv4Address ip, const MacAddress& mac, VlanId vlan) noexcept {
  NeighborSlot* slot =
      probe(t.neighbors, neighbor_hash(ip), [&](const NeighborSlot& s) { return s.ip == ip; });
  const bool is_new = slot != nullptr && (slot->flags & slot_flag::kUsed) == 0;
  if (slot == nullptr || (is_new && t.neighbor_count >= kNeighborSlots / 2)) {
    return false;
  }
  *slot = NeighborSlot{ip, mac, vlan, slot_flag::kUsed};
  t.neighbor_count += is_new ? 1U : 0U;
  return true;
}

bool route_insert(Tables& t, const RouteEntry& route) noexcept {
  if (t.num_routes >= kMaxRoutes) {
    return false;
  }
  // Insertion sort: keep longest prefixes first; equal lengths keep insertion order.
  std::size_t pos = t.num_routes;
  while (pos > 0 && t.routes[pos - 1].prefix.length < route.prefix.length) {
    t.routes[pos] = t.routes[pos - 1];
    --pos;
  }
  t.routes[pos] = route;
  ++t.num_routes;
  return true;
}

}  // namespace softswitch::shm
