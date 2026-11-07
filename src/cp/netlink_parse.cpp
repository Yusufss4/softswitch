// Pure parsing of rtnetlink link messages (no sockets), so it can be unit-tested and fuzzed.
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cstring>

#include "softswitch/cp/netlink.hpp"

namespace softswitch::cp {
namespace {

template <typename T>
bool read_struct(std::span<const std::uint8_t> bytes, std::size_t offset, T& out) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

constexpr std::size_t align4(std::size_t n) noexcept { return (n + 3U) & ~std::size_t{3}; }

void parse_attributes(std::span<const std::uint8_t> attrs, LinkEvent& ev) {
  std::size_t off = 0;
  while (off + sizeof(rtattr) <= attrs.size()) {
    rtattr rta{};
    (void)read_struct(attrs, off, rta);
    if (rta.rta_len < sizeof(rtattr) || rta.rta_len > attrs.size() - off) {
      return;  // malformed: stop rather than guess
    }
    const auto payload = attrs.subspan(off + sizeof(rtattr), rta.rta_len - sizeof(rtattr));
    if (rta.rta_type == IFLA_IFNAME) {
      const auto* text = reinterpret_cast<const char*>(payload.data());
      ev.name.assign(text, ::strnlen(text, payload.size()));
    } else if (rta.rta_type == IFLA_ADDRESS && payload.size() == 6) {
      ev.mac = MacAddress::from_bytes(payload.data());
    }
    off += align4(rta.rta_len);
  }
}

}  // namespace

std::vector<LinkEvent> parse_link_messages(std::span<const std::uint8_t> datagram) {
  std::vector<LinkEvent> events;
  std::size_t off = 0;
  while (off + sizeof(nlmsghdr) <= datagram.size()) {
    nlmsghdr nh{};
    (void)read_struct(datagram, off, nh);
    if (nh.nlmsg_len < sizeof(nlmsghdr) || nh.nlmsg_len > datagram.size() - off) {
      break;
    }
    const auto msg = datagram.subspan(off, nh.nlmsg_len);
    if ((nh.nlmsg_type == RTM_NEWLINK || nh.nlmsg_type == RTM_DELLINK)) {
      ifinfomsg ifi{};
      if (read_struct(msg, sizeof(nlmsghdr), ifi)) {
        LinkEvent ev;
        ev.ifindex = ifi.ifi_index;
        ev.admin_up = (ifi.ifi_flags & IFF_UP) != 0;
        ev.carrier = (ifi.ifi_flags & IFF_LOWER_UP) != 0;
        ev.deleted = nh.nlmsg_type == RTM_DELLINK;
        const std::size_t attrs_at = sizeof(nlmsghdr) + align4(sizeof(ifinfomsg));
        if (attrs_at <= msg.size()) {
          parse_attributes(msg.subspan(attrs_at), ev);
        }
        events.push_back(std::move(ev));
      }
    }
    off += align4(nh.nlmsg_len);
  }
  return events;
}

}  // namespace softswitch::cp
