#include "softswitch/pkt/icmp.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/ipv4.hpp"

namespace softswitch::pkt {
namespace {
constexpr std::uint8_t kDefaultTtl = 64;
}

std::optional<IcmpHeader> parse_icmp(std::span<const std::uint8_t> l4) noexcept {
  if (l4.size() < kIcmpHeaderLen) {
    return std::nullopt;
  }
  const std::uint8_t* p = l4.data();
  return IcmpHeader{p[0], p[1], load_be16(p + 2), load_be16(p + 4), load_be16(p + 6)};
}

bool echo_request_to_reply(std::span<std::uint8_t> frame) noexcept {
  const auto eth = parse_ethernet(frame);
  if (!eth || eth->tagged || eth->ethertype != kEtherTypeIpv4) {
    return false;
  }
  auto l3 = frame.subspan(eth->payload_offset);
  const auto ip = parse_ipv4(l3);
  if (!ip || ip->protocol != kIpProtoIcmp) {
    return false;
  }
  auto l4 = l3.subspan(ip->header_len, ip->total_len - ip->header_len);
  const auto icmp = parse_icmp(l4);
  if (!icmp || icmp->type != kIcmpEchoRequest || icmp->code != 0) {
    return false;
  }

  // Ethernet: swap addresses.
  std::array<std::uint8_t, 6> tmp{};
  std::memcpy(tmp.data(), frame.data(), tmp.size());
  std::memcpy(frame.data(), frame.data() + 6, tmp.size());
  std::memcpy(frame.data() + 6, tmp.data(), tmp.size());

  // IPv4: swap addresses, fresh TTL, recompute checksum.
  std::uint8_t* p = l3.data();
  store_be32(p + 12, ip->dst.value);
  store_be32(p + 16, ip->src.value);
  p[8] = kDefaultTtl;
  store_be16(p + 10, 0);
  store_be16(p + 10, internet_checksum(l3.first(ip->header_len)));

  // ICMP: echo reply, recompute checksum over the whole message.
  l4[0] = kIcmpEchoReply;
  store_be16(l4.data() + 2, 0);
  store_be16(l4.data() + 2, internet_checksum(l4));
  return true;
}

std::size_t build_icmp_error_frame(std::span<std::uint8_t> out, const MacAddress& dst,
                                   const MacAddress& src, Ipv4Address from, std::uint8_t type,
                                   std::uint8_t code,
                                   std::span<const std::uint8_t> offending) noexcept {
  const auto orig = parse_ipv4(offending);
  if (!orig) {
    return 0;
  }
  const std::size_t quote = std::min<std::size_t>(orig->total_len, orig->header_len + 8U);
  const std::size_t ip_len = kIpv4MinHeaderLen + kIcmpHeaderLen + quote;
  if (out.size() < std::max(kEthHeaderLen + ip_len, kMinFrameLen)) {
    return 0;
  }
  write_eth_header(out, dst, src, kEtherTypeIpv4);
  auto l3 = out.subspan(kEthHeaderLen);
  Ipv4Header h;
  h.total_len = static_cast<std::uint16_t>(ip_len);
  h.ttl = kDefaultTtl;
  h.protocol = kIpProtoIcmp;
  h.src = from;
  h.dst = orig->src;
  write_ipv4_header(l3, h);

  auto l4 = l3.subspan(kIpv4MinHeaderLen, kIcmpHeaderLen + quote);
  l4[0] = type;
  l4[1] = code;
  std::memset(l4.data() + 2, 0, 6);
  std::memcpy(l4.data() + kIcmpHeaderLen, offending.data(), quote);
  store_be16(l4.data() + 2, internet_checksum(l4));
  return pad_frame(out, kEthHeaderLen + ip_len);
}

}  // namespace softswitch::pkt
