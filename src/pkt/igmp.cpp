#include "softswitch/pkt/igmp.hpp"

#include <algorithm>

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::pkt {
namespace {
constexpr std::size_t kIgmpV2Len = 8;
constexpr std::size_t kIgmpV3ReportHeaderLen = 8;
constexpr std::size_t kIgmpV3RecordHeaderLen = 8;
constexpr std::size_t kRouterAlertLen = 4;
}  // namespace

bool IgmpMessage::record_is_join(const IgmpGroupRecord& r) noexcept {
  switch (r.type) {
    case kIgmpModeIsExclude:
    case kIgmpChangeToExclude:
    case kIgmpAllowNewSources:
      return true;
    case kIgmpModeIsInclude:
    case kIgmpChangeToInclude:
      return r.num_sources > 0;  // INCLUDE{} means "leave"
    default:
      return false;
  }
}

std::optional<IgmpMessage> parse_igmp(std::span<const std::uint8_t> l4) noexcept {
  if (l4.size() < kIgmpV2Len || internet_checksum(l4) != 0) {
    return std::nullopt;
  }
  const std::uint8_t* p = l4.data();
  IgmpMessage msg;
  msg.type = p[0];
  msg.max_resp = p[1];
  switch (msg.type) {
    case kIgmpQuery:
    case kIgmpV1Report:
    case kIgmpV2Report:
    case kIgmpV2Leave:
      msg.group = Ipv4Address{load_be32(p + 4)};
      return msg;
    case kIgmpV3Report:
      break;
    default:
      return std::nullopt;
  }

  const std::uint16_t count = load_be16(p + 6);
  std::size_t offset = kIgmpV3ReportHeaderLen;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (l4.size() - offset < kIgmpV3RecordHeaderLen) {
      return std::nullopt;
    }
    const std::uint8_t* r = p + offset;
    IgmpGroupRecord rec{r[0], load_be16(r + 2), Ipv4Address{load_be32(r + 4)}};
    const std::size_t len =
        kIgmpV3RecordHeaderLen + (std::size_t{rec.num_sources} * 4U) + (std::size_t{r[1]} * 4U);
    if (l4.size() - offset < len) {
      return std::nullopt;
    }
    if (msg.num_records < kMaxIgmpRecords) {
      msg.records[msg.num_records++] = rec;
    }
    offset += len;
  }
  return msg;
}

std::size_t build_igmp_query_frame(std::span<std::uint8_t> out, const MacAddress& src_mac,
                                   Ipv4Address src_ip, Ipv4Address group,
                                   std::uint8_t max_resp_tenths) noexcept {
  const Ipv4Address dst_ip = group.is_zero() ? kAllHostsGroup : group;
  const std::size_t ip_header_len = kIpv4MinHeaderLen + kRouterAlertLen;
  const std::size_t frame_len = kEthHeaderLen + ip_header_len + kIgmpV2Len;
  if (out.size() < std::max(frame_len, kMinFrameLen)) {
    return 0;
  }
  write_eth_header(out, multicast_mac(dst_ip), src_mac, kEtherTypeIpv4);

  std::uint8_t* ip = out.data() + kEthHeaderLen;
  ip[0] = 0x46;  // version 4, IHL 6 (Router Alert option)
  ip[1] = 0xc0;  // DSCP CS6 (internetwork control)
  store_be16(ip + 2, static_cast<std::uint16_t>(ip_header_len + kIgmpV2Len));
  store_be32(ip + 4, 0);
  ip[8] = 1;  // TTL
  ip[9] = kIpProtoIgmp;
  store_be16(ip + 10, 0);
  store_be32(ip + 12, src_ip.value);
  store_be32(ip + 16, dst_ip.value);
  store_be32(ip + 20, 0x94040000U);  // Router Alert (RFC 2113)
  store_be16(ip + 10, internet_checksum(out.subspan(kEthHeaderLen, ip_header_len)));

  std::uint8_t* igmp = ip + ip_header_len;
  igmp[0] = kIgmpQuery;
  igmp[1] = max_resp_tenths;
  store_be16(igmp + 2, 0);
  store_be32(igmp + 4, group.value);
  store_be16(igmp + 2, internet_checksum(out.subspan(kEthHeaderLen + ip_header_len, kIgmpV2Len)));
  return pad_frame(out, frame_len);
}

}  // namespace softswitch::pkt
