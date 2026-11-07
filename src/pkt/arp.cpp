#include "softswitch/pkt/arp.hpp"

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"

namespace softswitch::pkt {
namespace {
constexpr std::uint16_t kHwEthernet = 1;
constexpr std::uint8_t kHwLen = 6;
constexpr std::uint8_t kProtoLen = 4;
}  // namespace

std::optional<ArpPacket> parse_arp(std::span<const std::uint8_t> payload) noexcept {
  if (payload.size() < kArpLen) {
    return std::nullopt;
  }
  const std::uint8_t* p = payload.data();
  if (load_be16(p) != kHwEthernet || load_be16(p + 2) != kEtherTypeIpv4 || p[4] != kHwLen ||
      p[5] != kProtoLen) {
    return std::nullopt;
  }
  ArpPacket arp;
  arp.op = load_be16(p + 6);
  arp.sender_mac = MacAddress::from_bytes(p + 8);
  arp.sender_ip = Ipv4Address{load_be32(p + 14)};
  arp.target_mac = MacAddress::from_bytes(p + 18);
  arp.target_ip = Ipv4Address{load_be32(p + 24)};
  return arp;
}

std::size_t build_arp_frame(std::span<std::uint8_t> out, const MacAddress& dst,
                            const MacAddress& src, const ArpPacket& arp) noexcept {
  if (out.size() < kMinFrameLen) {
    return 0;
  }
  write_eth_header(out, dst, src, kEtherTypeArp);
  std::uint8_t* p = out.data() + kEthHeaderLen;
  store_be16(p, kHwEthernet);
  store_be16(p + 2, kEtherTypeIpv4);
  p[4] = kHwLen;
  p[5] = kProtoLen;
  store_be16(p + 6, arp.op);
  arp.sender_mac.write_to(p + 8);
  store_be32(p + 14, arp.sender_ip.value);
  arp.target_mac.write_to(p + 18);
  store_be32(p + 24, arp.target_ip.value);
  return pad_frame(out, kEthHeaderLen + kArpLen);
}

}  // namespace softswitch::pkt
