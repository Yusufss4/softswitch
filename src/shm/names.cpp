#include "softswitch/shm/region.hpp"

namespace softswitch::shm {

const char* to_string(Decision decision) noexcept {
  switch (decision) {
    case Decision::kForward:
      return "forward";
    case Decision::kFlood:
      return "flood";
    case Decision::kMulticast:
      return "multicast";
    case Decision::kRouted:
      return "routed";
    case Decision::kPunt:
      return "punt";
    case Decision::kDrop:
      return "drop";
  }
  return "?";
}

const char* to_string(DropReason reason) noexcept {
  switch (reason) {
    case DropReason::kNone:
      return "none";
    case DropReason::kPortDown:
      return "port-down";
    case DropReason::kMalformed:
      return "malformed";
    case DropReason::kUntaggedNotAllowed:
      return "untagged-not-allowed";
    case DropReason::kTaggedOnAccess:
      return "tagged-on-access";
    case DropReason::kVlanNotMember:
      return "vlan-not-member";
    case DropReason::kStpBlocked:
      return "stp-blocked";
    case DropReason::kReservedMulticast:
      return "reserved-multicast";
    case DropReason::kSameSegment:
      return "same-segment";
    case DropReason::kNoEgress:
      return "no-egress";
    case DropReason::kNoRoute:
      return "no-route";
    case DropReason::kPuntRingFull:
      return "punt-ring-full";
    case DropReason::kNoListeners:
      return "no-listeners";
    case DropReason::kCount:
      break;
  }
  return "?";
}

const char* to_string(StpState state) noexcept {
  switch (state) {
    case StpState::kDisabled:
      return "disabled";
    case StpState::kDiscarding:
      return "discarding";
    case StpState::kLearning:
      return "learning";
    case StpState::kForwarding:
      return "forwarding";
  }
  return "?";
}

const char* to_string(PublishMode mode) noexcept {
  switch (mode) {
    case PublishMode::kQsbr:
      return "qsbr";
    case PublishMode::kMutex:
      return "mutex";
    case PublishMode::kMutexPi:
      return "mutex-pi";
  }
  return "?";
}

std::optional<PublishMode> parse_publish_mode(std::string_view name) noexcept {
  if (name == "qsbr") {
    return PublishMode::kQsbr;
  }
  if (name == "mutex") {
    return PublishMode::kMutex;
  }
  if (name == "mutex-pi") {
    return PublishMode::kMutexPi;
  }
  return std::nullopt;
}

}  // namespace softswitch::shm
