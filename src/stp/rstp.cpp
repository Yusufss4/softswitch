#include "softswitch/stp/rstp.hpp"

#include <stdexcept>
#include <utility>

namespace softswitch::stp {
namespace {

constexpr std::uint16_t to_bpdu_time(std::chrono::seconds s) noexcept {
  return static_cast<std::uint16_t>(s.count() * 256);
}

}  // namespace

const char* to_string(Role role) noexcept {
  switch (role) {
    case Role::kDisabled:
      return "disabled";
    case Role::kRoot:
      return "root";
    case Role::kDesignated:
      return "designated";
    case Role::kAlternate:
      return "alternate";
    case Role::kBackup:
      return "backup";
  }
  return "?";
}

const char* to_string(State state) noexcept {
  switch (state) {
    case State::kDiscarding:
      return "discarding";
    case State::kLearning:
      return "learning";
    case State::kForwarding:
      return "forwarding";
  }
  return "?";
}

Rstp::Rstp(const BridgeConfig& config, std::size_t num_ports, Callbacks callbacks)
    : config_(config),
      bridge_id_(make_bridge_id(config_.priority, config_.mac)),
      callbacks_(std::move(callbacks)),
      ports_(num_ports),
      root_{bridge_id_, 0, bridge_id_, 0} {
  if (num_ports == 0 || num_ports > 0xFFF) {
    throw std::invalid_argument("RSTP needs 1..4095 ports");
  }
  for (PortId p = 0; p < port_count(); ++p) {
    ports_[p].status.port_id = port_id(p);
  }
}

std::uint16_t Rstp::port_id(PortId p) const noexcept {
  const unsigned priority = ports_[p].config.priority & 0xF0U;
  return static_cast<std::uint16_t>((priority << 8U) | ((p + 1U) & 0x0FFFU));
}

PriorityVector Rstp::designated_vector(PortId p) const noexcept {
  return PriorityVector{root_.root, root_.cost, bridge_id_, port_id(p)};
}

void Rstp::set_port_config(PortId p, const PortConfig& config, Clock::time_point now) {
  Port& port = ports_.at(p);
  port.config = config;
  port.status.port_id = port_id(p);
  port.status.oper_edge = config.edge;
  reselect(now);
  transmit_pending(now);
}

void Rstp::set_port_enabled(PortId p, bool enabled, Clock::time_point now) {
  Port& port = ports_.at(p);
  if (port.status.enabled == enabled) {
    return;
  }
  port.status.enabled = enabled;
  port.status.received.reset();
  port.status.proposing = false;
  port.status.oper_edge = port.config.edge;
  if (!enabled && callbacks_.flush) {
    callbacks_.flush(kInvalidPort);  // MACs behind the dead link are gone; relearn everywhere
  }
  reselect(now);
  transmit_pending(now);
}

void Rstp::set_bridge_priority(std::uint16_t priority, Clock::time_point now) {
  config_.priority = priority;
  bridge_id_ = make_bridge_id(priority, config_.mac);
  reselect(now);
  for (auto& port : ports_) {
    if (port.status.role == Role::kDesignated) {
      port.send_pending = true;
    }
  }
  transmit_pending(now);
}

void Rstp::receive(PortId p, const pkt::Bpdu& bpdu, Clock::time_point now) {
  Port& port = ports_.at(p);
  if (!port.status.enabled || bpdu.type == pkt::kBpduTypeTcn) {
    return;
  }
  ++port.status.bpdus_received;
  port.status.oper_edge = false;  // a bridge is behind this port after all
  if (bpdu.message_age >= bpdu.max_age && bpdu.max_age != 0) {
    return;  // travelled too far
  }

  const PriorityVector vector{bpdu.root_id, bpdu.root_path_cost, bpdu.bridge_id, bpdu.port_id};
  const pkt::BpduRole sender_role =
      bpdu.type == pkt::kBpduTypeConfig ? pkt::BpduRole::kDesignated : bpdu.role();

  if (sender_role == pkt::BpduRole::kDesignated) {
    // Designated information from the neighbour: accept it if it is better than what we hold,
    // or a (possibly worse) update from the same designated bridge and port.
    const auto& held = port.status.received;
    const bool same_sender = held && held->bridge == vector.bridge && held->port == vector.port;
    const bool better_than_ours = vector < designated_vector(p);
    if (better_than_ours || same_sender || (held && vector < *held)) {
      port.status.received = vector;
      port.message_age = bpdu.message_age;
      port.info_expires = now + (3 * config_.hello_time);
    }
    const auto old_root_port = root_port_;
    reselect(now);
    if (port.status.role == Role::kDesignated && port.status.proposing) {
      // The neighbour just told us it is (wrongly) designated -- typically it has only now come
      // up. Re-propose right away instead of waiting up to a hello for the next attempt.
      port.send_pending = true;
    }
    if (bpdu.has(pkt::bpdu_flag::kProposal)) {
      if (port.status.role == Role::kRoot) {
        // Sync: block our designated ports (they re-propose), then agree upstream.
        if (old_root_port == root_port_) {
          sync_designated_ports(p, now);
        }
        port.send_agreement = true;
      } else if (port.status.role == Role::kAlternate || port.status.role == Role::kBackup) {
        port.send_agreement = true;  // discarding ports are always in sync
      }
    }
  } else if (bpdu.has(pkt::bpdu_flag::kAgreement) && agreement_is_valid(bpdu) &&
             port.status.role == Role::kDesignated && port.status.proposing) {
    // Our proposal was accepted downstream: forward immediately.
    port.status.proposing = false;
    set_state(p, State::kForwarding, now);
  }

  if (bpdu.has(pkt::bpdu_flag::kTopologyChange) &&
      (port.status.role == Role::kRoot || port.status.role == Role::kDesignated)) {
    topology_change(p, false, now);
  }
  transmit_pending(now);
}

bool Rstp::agreement_is_valid(const pkt::Bpdu& bpdu) const noexcept {
  if (bpdu.root_id != root_.root) {
    return false;
  }
  // An alternate/backup port is discarding, so agreeing can never close a loop.
  if (bpdu.role() == pkt::BpduRole::kAlternateBackup) {
    return true;
  }
  // A root port's agreement only counts if that bridge really is downstream of us: its path to
  // the root must be longer than ours. After the root bridge disappears, stale information can
  // circulate (RSTP count-to-infinity); agreements based on it would otherwise open every port
  // around a cycle and form a forwarding loop.
  return bpdu.root_path_cost > root_.cost;
}

void Rstp::tick(Clock::time_point now) {
  bool changed = false;
  for (Port& port : ports_) {
    if (!port.status.enabled) {
      continue;
    }
    // Received information times out after 3 hellos without a BPDU.
    if (port.status.received && now >= port.info_expires) {
      port.status.received.reset();
      changed = true;
    }
  }
  if (changed) {
    reselect(now);
  }
  for (PortId p = 0; p < port_count(); ++p) {
    Port& port = ports_[p];
    if (!port.status.enabled) {
      continue;
    }
    // Forward-delay fallback for designated ports that never got an agreement.
    if (port.status.role == Role::kDesignated && port.status.proposing && now >= port.fd_while) {
      if (port.status.state == State::kDiscarding) {
        set_state(p, State::kLearning, now);
        port.fd_while = now + config_.forward_delay;
      } else {
        port.status.proposing = false;
        set_state(p, State::kForwarding, now);
      }
    }
    // Designated ports send BPDUs every hello time, and repeat outstanding proposals sooner.
    if (port.status.role == Role::kDesignated &&
        (now >= port.next_hello ||
         (port.status.proposing && now >= port.last_tx + kProposalRetry))) {
      port.send_pending = true;
    }
  }
  transmit_pending(now);
}

void Rstp::reselect(Clock::time_point now) {
  // 1. Root priority vector: our own, or the best one offered through any port.
  PriorityVector best{bridge_id_, 0, bridge_id_, 0};
  std::optional<PortId> best_port;
  std::uint16_t best_local = 0xFFFF;
  for (PortId p = 0; p < port_count(); ++p) {
    const Port& port = ports_[p];
    if (!port.status.enabled || !port.status.received ||
        port.status.received->bridge == bridge_id_) {
      continue;  // nothing fresh, or our own BPDU looped back (backup port)
    }
    const PriorityVector& r = *port.status.received;
    const PriorityVector candidate{r.root, r.cost + port.config.path_cost, r.bridge, r.port};
    if (candidate < best || (best_port && candidate == best && port_id(p) < best_local)) {
      best = candidate;
      best_port = p;
      best_local = port_id(p);
    }
  }
  // Downstream bridges must hear about any change of root information at once -- a new root
  // or cost through the *same* root port matters just as much as a new root port.
  const bool root_info_changed = best_port != root_port_ || best != root_;
  root_ = best;
  root_port_ = best_port;
  root_message_age_ =
      best_port ? static_cast<std::uint16_t>(ports_[*best_port].message_age + 256U) : 0;

  // 2. Port roles, all decided before any state changes so that sync sees the final roles.
  std::vector<Role> old_roles(ports_.size());
  for (PortId p = 0; p < port_count(); ++p) {
    PortStatus& st = ports_[p].status;
    old_roles[p] = st.role;
    Role role = Role::kDesignated;
    if (!st.enabled) {
      role = Role::kDisabled;
    } else if (root_port_ == p) {
      role = Role::kRoot;
    } else if (st.received && *st.received < designated_vector(p)) {
      role = st.received->bridge == bridge_id_ ? Role::kBackup : Role::kAlternate;
    }
    st.role = role;
    if (role == Role::kDesignated && old_roles[p] != Role::kDesignated) {
      st.received.reset();  // infoIs = Mine: what the neighbour told us no longer applies
      ports_[p].send_pending = true;
    }
  }

  // 3. A new root port forwards at once (rapid transition), but first every designated port --
  //    including a former root port -- is blocked until it re-agrees downstream (sync).
  if (root_port_ && old_roles[*root_port_] != Role::kRoot) {
    sync_designated_ports(*root_port_, now);
  }

  // 4. State transitions.
  for (PortId p = 0; p < port_count(); ++p) {
    Port& port = ports_[p];
    PortStatus& st = port.status;
    switch (st.role) {
      case Role::kDisabled:
      case Role::kAlternate:
      case Role::kBackup:
        st.proposing = false;
        set_state(p, State::kDiscarding, now);
        break;
      case Role::kRoot:
        st.proposing = false;
        set_state(p, State::kForwarding, now);
        break;
      case Role::kDesignated:
        if (st.oper_edge) {
          set_state(p, State::kForwarding, now);
        } else if (st.state != State::kForwarding && !st.proposing) {
          st.proposing = true;
          port.fd_while = now + config_.forward_delay;
          port.send_pending = true;
        }
        break;
    }
  }
  if (root_info_changed) {
    // Everyone downstream needs our new root information right away.
    for (auto& port : ports_) {
      if (port.status.role == Role::kDesignated) {
        port.send_pending = true;
      }
    }
  }
}

void Rstp::sync_designated_ports(PortId except, Clock::time_point now) {
  for (PortId q = 0; q < port_count(); ++q) {
    Port& port = ports_[q];
    if (q == except || port.status.role != Role::kDesignated || port.status.oper_edge) {
      continue;
    }
    set_state(q, State::kDiscarding, now);
    port.status.proposing = true;
    port.fd_while = now + config_.forward_delay;
    port.send_pending = true;
  }
}

void Rstp::set_state(PortId p, State state, Clock::time_point now) {
  PortStatus& st = ports_[p].status;
  if (st.state == state) {
    return;
  }
  st.state = state;
  if (callbacks_.state_changed) {
    callbacks_.state_changed(p, state);
  }
  if (state == State::kForwarding && !st.oper_edge) {
    topology_change(p, true, now);
  }
}

void Rstp::topology_change(PortId origin, bool detected_here, Clock::time_point now) {
  // A received TC repeats in every BPDU for 2 x hello, so count it once per window. But flush
  // and propagate on every one, as the 802.1D-2004 Topology Change machine does: during a
  // multi-step reconvergence a second, unrelated change can arrive inside the window, and
  // ignoring it leaves MACs pointing into a dead end until the next change.
  const bool repeat = !detected_here && last_tc_ && now < *last_tc_ + (2 * config_.hello_time);
  if (!repeat) {
    ++topology_changes_;
    last_tc_ = now;
  }
  if (callbacks_.flush) {
    callbacks_.flush(origin);
  }
  // Tell the rest of the tree: TC flags on our root and designated ports for 2 x hello.
  for (PortId q = 0; q < port_count(); ++q) {
    Port& port = ports_[q];
    if ((q == origin && !detected_here) || port.status.oper_edge || !port.status.enabled ||
        (port.status.role != Role::kRoot && port.status.role != Role::kDesignated)) {
      continue;
    }
    if (now >= port.tc_while) {
      port.tc_while = now + (2 * config_.hello_time);
      port.send_pending = true;
    }
  }
}

void Rstp::transmit(PortId p, Clock::time_point now) {
  Port& port = ports_[p];
  PortStatus& st = port.status;
  if (!callbacks_.send_bpdu || !st.enabled) {
    return;
  }
  pkt::Bpdu bpdu;
  bpdu.version = pkt::kBpduVersionRstp;
  bpdu.type = pkt::kBpduTypeRst;
  bpdu.root_id = root_.root;
  bpdu.root_path_cost = root_.cost;
  bpdu.bridge_id = bridge_id_;
  bpdu.port_id = port_id(p);
  bpdu.message_age = root_message_age_;
  bpdu.max_age = to_bpdu_time(config_.max_age);
  bpdu.hello_time = to_bpdu_time(config_.hello_time);
  bpdu.forward_delay = to_bpdu_time(config_.forward_delay);
  std::uint8_t flags = 0;
  switch (st.role) {
    case Role::kRoot:
      bpdu.set_role(pkt::BpduRole::kRoot);
      break;
    case Role::kAlternate:
    case Role::kBackup:
      bpdu.set_role(pkt::BpduRole::kAlternateBackup);
      break;
    default:
      bpdu.set_role(pkt::BpduRole::kDesignated);
      if (st.proposing) {
        flags |= pkt::bpdu_flag::kProposal;
      }
      break;
  }
  if (port.send_agreement) {
    flags |= pkt::bpdu_flag::kAgreement;
  }
  if (st.state != State::kDiscarding) {
    flags |= pkt::bpdu_flag::kLearning;
  }
  if (st.state == State::kForwarding) {
    flags |= pkt::bpdu_flag::kForwarding;
  }
  if (now < port.tc_while) {
    flags |= pkt::bpdu_flag::kTopologyChange;
  }
  bpdu.flags = static_cast<std::uint8_t>(bpdu.flags | flags);
  callbacks_.send_bpdu(p, bpdu);
  ++st.bpdus_sent;
  port.send_pending = false;
  port.send_agreement = false;
  port.next_hello = now + config_.hello_time;
  port.last_tx = now;
}

void Rstp::transmit_pending(Clock::time_point now) {
  for (PortId p = 0; p < port_count(); ++p) {
    const Port& port = ports_[p];
    // Designated ports talk; root ports only answer proposals and relay topology changes.
    const bool talker = port.status.role == Role::kDesignated ||
                        (port.status.role == Role::kRoot && now < port.tc_while);
    if (port.status.enabled && (port.send_agreement || (port.send_pending && talker))) {
      transmit(p, now);
    }
  }
}

}  // namespace softswitch::stp
