#include "softswitch/pkt/ethernet.hpp"

#include <algorithm>
#include <cstring>

#include "softswitch/pkt/bytes.hpp"

namespace softswitch::pkt {

std::optional<EthernetView> parse_ethernet(std::span<const std::uint8_t> frame) noexcept {
  if (frame.size() < kEthHeaderLen) {
    return std::nullopt;
  }
  const std::uint8_t* p = frame.data();
  EthernetView view;
  view.dst = MacAddress::from_bytes(p);
  view.src = MacAddress::from_bytes(p + 6);
  std::uint16_t type = load_be16(p + kMacAddrsLen);
  std::size_t offset = kEthHeaderLen;
  if (type == kEtherTypeVlan) {
    if (frame.size() < kEthHeaderLen + kVlanTagLen) {
      return std::nullopt;
    }
    view.tagged = true;
    view.tci = load_be16(p + kEthHeaderLen);
    type = load_be16(p + kEthHeaderLen + 2);
    offset += kVlanTagLen;
  }
  view.ethertype = type;
  view.payload_offset = offset;
  return view;
}

std::size_t write_eth_header(std::span<std::uint8_t> out, const MacAddress& dst,
                             const MacAddress& src, std::uint16_t ethertype) noexcept {
  if (out.size() < kEthHeaderLen) {
    return 0;
  }
  dst.write_to(out.data());
  src.write_to(out.data() + 6);
  store_be16(out.data() + kMacAddrsLen, ethertype);
  return kEthHeaderLen;
}

std::size_t copy_with_tag(std::span<const std::uint8_t> untagged, std::uint16_t tci,
                          std::span<std::uint8_t> out) noexcept {
  if (untagged.size() < kEthHeaderLen || out.size() < untagged.size() + kVlanTagLen) {
    return 0;
  }
  std::uint8_t* o = out.data();
  std::memcpy(o, untagged.data(), kMacAddrsLen);
  store_be16(o + kMacAddrsLen, kEtherTypeVlan);
  store_be16(o + kMacAddrsLen + 2, tci);
  std::memcpy(o + kEthHeaderLen + 2, untagged.data() + kMacAddrsLen,
              untagged.size() - kMacAddrsLen);
  return untagged.size() + kVlanTagLen;
}

std::size_t insert_tag(std::span<std::uint8_t> buf, std::size_t len, std::uint16_t tci) noexcept {
  if (len < kEthHeaderLen || len > buf.size() || buf.size() - len < kVlanTagLen) {
    return 0;
  }
  std::uint8_t* p = buf.data();
  std::memmove(p + kMacAddrsLen + kVlanTagLen, p + kMacAddrsLen, len - kMacAddrsLen);
  store_be16(p + kMacAddrsLen, kEtherTypeVlan);
  store_be16(p + kMacAddrsLen + 2, tci);
  return len + kVlanTagLen;
}

std::size_t strip_tag(std::span<std::uint8_t> frame) noexcept {
  if (frame.size() < kEthHeaderLen + kVlanTagLen ||
      load_be16(frame.data() + kMacAddrsLen) != kEtherTypeVlan) {
    return 0;
  }
  std::uint8_t* p = frame.data();
  std::memmove(p + kMacAddrsLen, p + kMacAddrsLen + kVlanTagLen,
               frame.size() - kMacAddrsLen - kVlanTagLen);
  return frame.size() - kVlanTagLen;
}

std::size_t pad_frame(std::span<std::uint8_t> buf, std::size_t len) noexcept {
  const std::size_t target = std::min(buf.size(), kMinFrameLen);
  if (len >= target) {
    return len;
  }
  std::memset(buf.data() + len, 0, target - len);
  return target;
}

}  // namespace softswitch::pkt
