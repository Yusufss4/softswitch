#pragma once

/// @file
/// A reduced Rapid Spanning Tree (IEEE 802.1D-2004 clause 17) for one bridge.
///
/// Implemented: priority-vector role selection (root / designated / alternate / backup /
/// disabled), proposal/agreement with sync for sub-second convergence on point-to-point links,
/// instant alternate-to-root failover, forward-delay fallback when no agreement arrives,
/// received-information expiry (3 x hello), edge ports, and topology-change detection with
/// FDB flushing and TC propagation.
///
/// Left out: STP/RSTP version migration, point-to-point detection (every link is treated as
/// point-to-point), TCN BPDUs, and per-VLAN instances (one tree for all VLANs).
///
/// The class is pure: it never touches sockets. BPDUs go out through a callback, time comes in
/// as arguments, so it can be driven by a simulated network in tests.

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/pkt/bpdu.hpp"

namespace softswitch::stp {

using Clock = std::chrono::steady_clock;
using BridgeId = std::uint64_t;  ///< 16-bit priority, 48-bit MAC.

[[nodiscard]] constexpr BridgeId make_bridge_id(std::uint16_t priority,
                                                const MacAddress& mac) noexcept {
  return (std::uint64_t{priority} << 48U) | mac.to_u64();
}

enum class Role : std::uint8_t { kDisabled, kRoot, kDesignated, kAlternate, kBackup };
enum class State : std::uint8_t { kDiscarding, kLearning, kForwarding };

[[nodiscard]] const char* to_string(Role role) noexcept;
[[nodiscard]] const char* to_string(State state) noexcept;

/// Spanning-tree priority vector; smaller is better (compared field by field).
struct PriorityVector {
  BridgeId root = 0;
  std::uint32_t cost = 0;
  BridgeId bridge = 0;     ///< Designated bridge.
  std::uint16_t port = 0;  ///< Designated port ID.
  friend constexpr auto operator<=>(const PriorityVector&, const PriorityVector&) = default;
};

struct BridgeConfig {
  std::uint16_t priority = 32768;  ///< Multiples of 4096.
  MacAddress mac;
  std::chrono::seconds hello_time{2};
  std::chrono::seconds max_age{20};
  std::chrono::seconds forward_delay{15};
};

struct PortConfig {
  std::uint32_t path_cost = 20000;  ///< 802.1D-2004 default for 1 Gb/s.
  std::uint8_t priority = 128;      ///< Multiples of 16.
  bool edge = false;                ///< Admin edge ("portfast"): no bridges behind this port.
};

struct PortStatus {
  Role role = Role::kDisabled;
  State state = State::kDiscarding;
  bool enabled = false;
  bool oper_edge = false;
  bool proposing = false;
  std::optional<PriorityVector> received;  ///< Neighbour's designated vector, if fresh.
  std::uint16_t port_id = 0;
  std::uint64_t bpdus_sent = 0;
  std::uint64_t bpdus_received = 0;
};

class Rstp {
 public:
  /// While a proposal is outstanding it is repeated this often (instead of once per hello), so a
  /// BPDU lost at link-up -- common while a link is still coming up -- costs < 1 s, not 2 s.
  static constexpr std::chrono::milliseconds kProposalRetry{500};

  struct Callbacks {
    /// Transmit @p bpdu on @p port.
    std::function<void(PortId, const pkt::Bpdu&)> send_bpdu;
    /// @p port moved to @p state; the switch must apply it to the data plane.
    std::function<void(PortId, State)> state_changed;
    /// Topology change: flush learned MACs on every port except @p except
    /// (kInvalidPort = all ports).
    std::function<void(PortId except)> flush;
  };

  Rstp(const BridgeConfig& config, std::size_t num_ports, Callbacks callbacks);

  void set_port_config(PortId port, const PortConfig& config, Clock::time_point now);
  /// Link up/down (or admin up/down) for @p port.
  void set_port_enabled(PortId port, bool enabled, Clock::time_point now);
  void set_bridge_priority(std::uint16_t priority, Clock::time_point now);
  /// Processes a BPDU received on @p port.
  void receive(PortId port, const pkt::Bpdu& bpdu, Clock::time_point now);
  /// Advances timers (hello transmission, information expiry, forward-delay fallback,
  /// topology-change timers). Call at least every 100 ms.
  void tick(Clock::time_point now);

  [[nodiscard]] BridgeId bridge_id() const noexcept { return bridge_id_; }
  [[nodiscard]] BridgeId root_id() const noexcept { return root_.root; }
  [[nodiscard]] std::uint32_t root_cost() const noexcept { return root_.cost; }
  [[nodiscard]] std::optional<PortId> root_port() const noexcept { return root_port_; }
  [[nodiscard]] bool is_root_bridge() const noexcept { return root_.root == bridge_id_; }
  [[nodiscard]] const PortStatus& status(PortId port) const { return ports_.at(port).status; }
  [[nodiscard]] const PortConfig& port_config(PortId port) const { return ports_.at(port).config; }
  [[nodiscard]] std::size_t num_ports() const noexcept { return ports_.size(); }
  [[nodiscard]] std::uint64_t topology_changes() const noexcept { return topology_changes_; }
  [[nodiscard]] std::optional<Clock::time_point> last_topology_change() const noexcept {
    return last_tc_;
  }
  [[nodiscard]] const BridgeConfig& config() const noexcept { return config_; }

 private:
  struct Port {
    PortConfig config;
    PortStatus status;
    std::uint16_t message_age = 0;  ///< Of the received info, in 1/256 s.
    Clock::time_point info_expires;
    Clock::time_point fd_while;  ///< Next forward-delay transition while proposing.
    Clock::time_point tc_while;  ///< Send TC flags until then.
    Clock::time_point next_hello;
    Clock::time_point last_tx;
    bool send_pending = false;
    bool send_agreement = false;
  };

  [[nodiscard]] PortId port_count() const noexcept { return static_cast<PortId>(ports_.size()); }
  [[nodiscard]] std::uint16_t port_id(PortId p) const noexcept;
  [[nodiscard]] PriorityVector designated_vector(PortId p) const noexcept;
  /// Re-selects roles and applies the resulting state transitions.
  void reselect(Clock::time_point now);
  void sync_designated_ports(PortId except, Clock::time_point now);
  [[nodiscard]] bool agreement_is_valid(const pkt::Bpdu& bpdu) const noexcept;
  void set_state(PortId p, State state, Clock::time_point now);
  void topology_change(PortId origin, bool detected_here, Clock::time_point now);
  void transmit(PortId p, Clock::time_point now);
  void transmit_pending(Clock::time_point now);

  BridgeConfig config_;
  BridgeId bridge_id_;
  Callbacks callbacks_;
  std::vector<Port> ports_;
  PriorityVector root_;
  std::optional<PortId> root_port_;
  std::uint16_t root_message_age_ = 0;
  std::uint64_t topology_changes_ = 0;
  std::optional<Clock::time_point> last_tc_;
};

}  // namespace softswitch::stp
