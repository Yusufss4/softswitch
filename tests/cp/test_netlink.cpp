#include <gtest/gtest.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cstring>
#include <string>
#include <vector>

#include "softswitch/cp/netlink.hpp"

namespace softswitch::cp {
namespace {

/// Builds RTM_NEWLINK/RTM_DELLINK messages the way the kernel lays them out.
class NetlinkBuilder {
 public:
  NetlinkBuilder& link(std::uint16_t type, int ifindex, unsigned flags, const std::string& name,
                       const std::vector<std::uint8_t>& mac = {}) {
    const std::size_t start = buf_.size();
    nlmsghdr nh{};
    nh.nlmsg_type = type;
    append(&nh, sizeof(nh));
    ifinfomsg ifi{};
    ifi.ifi_index = ifindex;
    ifi.ifi_flags = flags;
    append(&ifi, sizeof(ifi));
    attr(IFLA_IFNAME, name.c_str(), name.size() + 1);
    if (!mac.empty()) {
      attr(IFLA_ADDRESS, mac.data(), mac.size());
    }
    const auto len = static_cast<std::uint32_t>(buf_.size() - start);
    std::memcpy(buf_.data() + start, &len, sizeof(len));
    return *this;
  }
  [[nodiscard]] std::vector<std::uint8_t> build() const { return buf_; }

 private:
  void append(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
    buf_.resize((buf_.size() + 3U) & ~std::size_t{3});
  }
  void attr(std::uint16_t type, const void* data, std::size_t n) {
    rtattr rta{};
    rta.rta_type = type;
    rta.rta_len = static_cast<std::uint16_t>(sizeof(rta) + n);
    const std::size_t start = buf_.size();
    buf_.insert(buf_.end(), reinterpret_cast<const std::uint8_t*>(&rta),
                reinterpret_cast<const std::uint8_t*>(&rta) + sizeof(rta));
    const auto* b = static_cast<const std::uint8_t*>(data);
    buf_.insert(buf_.end(), b, b + n);
    buf_.resize(start + ((sizeof(rta) + n + 3U) & ~std::size_t{3}));
  }
  std::vector<std::uint8_t> buf_;
};

TEST(Netlink, ParsesLinkUpWithNameAndMac) {
  const auto msg = NetlinkBuilder{}
                       .link(RTM_NEWLINK, 7, IFF_UP | IFF_LOWER_UP | IFF_RUNNING, "s1p3",
                             {0x02, 0, 0, 0, 0, 0x33})
                       .build();
  const auto events = parse_link_messages(msg);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].ifindex, 7);
  EXPECT_EQ(events[0].name, "s1p3");
  EXPECT_EQ(events[0].mac.to_string(), "02:00:00:00:00:33");
  EXPECT_TRUE(events[0].oper_up());
}

TEST(Netlink, NoCarrierOrAdminDownIsNotOperUp) {
  const auto msg = NetlinkBuilder{}
                       .link(RTM_NEWLINK, 7, IFF_UP, "s1p3")        // peer gone: no carrier
                       .link(RTM_NEWLINK, 8, IFF_LOWER_UP, "s1p4")  // admin down
                       .link(RTM_DELLINK, 9, IFF_UP | IFF_LOWER_UP, "s1p5")
                       .build();
  const auto events = parse_link_messages(msg);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_FALSE(events[0].oper_up());
  EXPECT_TRUE(events[0].admin_up);
  EXPECT_FALSE(events[1].oper_up());
  EXPECT_TRUE(events[2].deleted);
  EXPECT_FALSE(events[2].oper_up());
}

TEST(Netlink, SkipsOtherMessageTypes) {
  auto msg = NetlinkBuilder{}.link(RTM_NEWLINK, 7, IFF_UP | IFF_LOWER_UP, "a").build();
  std::uint16_t done = NLMSG_DONE;
  std::memcpy(msg.data() + offsetof(nlmsghdr, nlmsg_type), &done, sizeof(done));
  EXPECT_TRUE(parse_link_messages(msg).empty());
}

TEST(Netlink, TruncatedInputIsRejectedSafely) {
  const auto msg = NetlinkBuilder{}.link(RTM_NEWLINK, 7, IFF_UP | IFF_LOWER_UP, "s1p3").build();
  for (std::size_t len = 0; len < msg.size(); ++len) {
    const auto events = parse_link_messages(std::span{msg}.first(len));
    EXPECT_TRUE(events.empty()) << "length " << len;
  }
}

TEST(Netlink, BogusAttributeLengthStopsAttributeParsing) {
  auto msg = NetlinkBuilder{}.link(RTM_NEWLINK, 7, IFF_UP | IFF_LOWER_UP, "s1p3").build();
  const std::size_t rta_at = sizeof(nlmsghdr) + sizeof(ifinfomsg);
  const std::uint16_t huge = 0xFFFF;
  std::memcpy(msg.data() + rta_at, &huge, sizeof(huge));
  const auto events = parse_link_messages(msg);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].name.empty());
  EXPECT_TRUE(events[0].oper_up());
}

}  // namespace
}  // namespace softswitch::cp
