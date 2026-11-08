#pragma once

/// @file
/// The control plane's IPv4 router: SVI addresses, the neighbour (ARP) cache, ARP and ICMP
/// responses, address resolution for the data plane, and the route table it publishes.
///
/// Pure: frames in, frames out. Every output is an untagged Ethernet frame to be forwarded
/// within a VLAN (the data plane picks the port from the FDB and applies the tag).

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "softswitch/common/types.hpp"

namespace softswitch::l3 {

using Clock = std::chrono::steady_clock;

struct Svi {
  VlanId vlan = 0;
  Ipv4Prefix address;  ///< e.g. 10.0.10.1/24
};

struct StaticRoute {
  Ipv4Prefix prefix;
  Ipv4Address nexthop;
  friend bool operator==(const StaticRoute&, const StaticRoute&) = default;
};

struct RouterConfig {
  MacAddress mac;  ///< Router MAC, shared by every SVI.
  std::vector<Svi> svis;
  std::vector<StaticRoute> static_routes;
  std::chrono::seconds arp_timeout{300};
};

struct Neighbor {
  MacAddress mac;
  VlanId vlan = 0;
  Clock::time_point updated;
};

struct Route {
  Ipv4Prefix prefix;  ///< Network form (host bits cleared).
  bool connected = false;
  VlanId vlan = 0;      ///< Egress VLAN.
  Ipv4Address nexthop;  ///< Unused when connected.
  bool active = false;  ///< A static route is inactive while its next hop isn't on any SVI.
};

/// A frame for the control plane to inject into @p vlan.
struct Output {
  VlanId vlan = 0;
  std::vector<std::uint8_t> frame;
};

class Router {
 public:
  /// ARP requests per unresolved address before giving up, one per second.
  static constexpr int kArpRetries = 3;
  /// Packets held per unresolved next hop while ARP is in flight.
  static constexpr std::size_t kMaxQueued = 4;

  explicit Router(RouterConfig config) : config_(std::move(config)) {}

  /// Replaces the configuration; neighbours outside every remaining SVI subnet are dropped.
  void set_config(RouterConfig config);
  [[nodiscard]] const RouterConfig& config() const noexcept { return config_; }

  /// An ARP frame seen in @p vlan. Learns the sender (RFC 826 merge), answers requests for our
  /// own address, and releases packets that were waiting for the sender.
  std::vector<Output> on_arp(VlanId vlan, std::span<const std::uint8_t> frame,
                             Clock::time_point now);
  /// An IPv4 packet addressed to one of our SVI addresses: answers ICMP echo requests.
  [[nodiscard]] std::vector<Output> on_to_router(VlanId vlan,
                                                 std::span<const std::uint8_t> frame) const;
  /// A packet whose TTL would expire if routed: answers with ICMP time exceeded.
  [[nodiscard]] std::vector<Output> on_ttl_expired(VlanId vlan,
                                                   std::span<const std::uint8_t> frame) const;
  /// The data plane could route @p frame but has no MAC for its next hop: queue it and ARP.
  std::vector<Output> on_neighbor_miss(VlanId vlan, std::span<const std::uint8_t> frame,
                                       Clock::time_point now);
  /// ARP retries and neighbour expiry.
  std::vector<Output> tick(Clock::time_point now);

  [[nodiscard]] const std::map<Ipv4Address, Neighbor>& neighbors() const noexcept {
    return neighbors_;
  }
  /// Connected routes for every SVI plus static routes resolved to an egress VLAN.
  [[nodiscard]] std::vector<Route> routes() const;
  /// True once since the last call if the neighbour table changed (needs publishing).
  [[nodiscard]] bool take_neighbors_changed() noexcept {
    const bool changed = neighbors_changed_;
    neighbors_changed_ = false;
    return changed;
  }
  void clear_neighbors();

  [[nodiscard]] const Svi* svi_in_vlan(VlanId vlan) const noexcept;
  [[nodiscard]] const Svi* svi_with_address(Ipv4Address ip) const noexcept;  ///< Our own IP.
  [[nodiscard]] const Svi* svi_containing(Ipv4Address ip) const noexcept;    ///< Connected subnet.
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }

 private:
  struct Pending {
    VlanId vlan = 0;  ///< Where to ARP.
    std::vector<std::vector<std::uint8_t>> queued;
    Clock::time_point next_try;
    int tries = 0;
  };

  void learn(Ipv4Address ip, const MacAddress& mac, VlanId vlan, Clock::time_point now,
             std::vector<Output>& out);
  [[nodiscard]] std::optional<Output> arp_request(const Svi& svi, Ipv4Address target) const;
  /// Rewrites a routable frame for its next hop (MACs, TTL, checksum).
  [[nodiscard]] static std::optional<Output> rewrite(std::vector<std::uint8_t> frame,
                                                     const Neighbor& n,
                                                     const MacAddress& router_mac);
  /// Next hop (connected destination or static gateway) and egress SVI for @p dst.
  [[nodiscard]] std::optional<std::pair<Ipv4Address, const Svi*>> resolve_nexthop(
      Ipv4Address dst) const;

  RouterConfig config_;
  std::map<Ipv4Address, Neighbor> neighbors_;
  std::map<Ipv4Address, Pending> pending_;
  bool neighbors_changed_ = false;
};

}  // namespace softswitch::l3
