#include "softswitch/pkt/ipv4.hpp"

#include "softswitch/pkt/bytes.hpp"

namespace softswitch::pkt {
namespace {

constexpr std::uint32_t fold(std::uint32_t sum) noexcept {
  while ((sum >> 16U) != 0) {
    sum = (sum & 0xffffU) + (sum >> 16U);
  }
  return sum;
}

constexpr std::size_t kChecksumOffset = 10;
constexpr std::size_t kTtlOffset = 8;

}  // namespace

std::optional<Ipv4Header> parse_ipv4(std::span<const std::uint8_t> l3) noexcept {
  if (l3.size() < kIpv4MinHeaderLen) {
    return std::nullopt;
  }
  const std::uint8_t* p = l3.data();
  if ((p[0] >> 4U) != 4) {
    return std::nullopt;
  }
  Ipv4Header h;
  h.header_len = static_cast<std::uint8_t>((p[0] & 0x0fU) * 4U);
  h.total_len = load_be16(p + 2);
  if (h.header_len < kIpv4MinHeaderLen || h.header_len > l3.size() || h.total_len < h.header_len ||
      h.total_len > l3.size()) {
    return std::nullopt;
  }
  h.tos = p[1];
  h.id = load_be16(p + 4);
  h.flags_frag = load_be16(p + 6);
  h.ttl = p[kTtlOffset];
  h.protocol = p[9];
  h.checksum = load_be16(p + kChecksumOffset);
  h.src = Ipv4Address{load_be32(p + 12)};
  h.dst = Ipv4Address{load_be32(p + 16)};
  return h;
}

bool ipv4_header_ok(std::span<const std::uint8_t> l3) noexcept {
  const auto h = parse_ipv4(l3);
  return h && internet_checksum(l3.first(h->header_len)) == 0;
}

std::uint16_t internet_checksum(std::span<const std::uint8_t> data,
                                std::uint32_t initial) noexcept {
  std::uint64_t sum = initial;
  std::size_t i = 0;
  for (; i + 1 < data.size(); i += 2) {
    sum += load_be16(data.data() + i);
  }
  if (i < data.size()) {
    sum += std::uint32_t{data[i]} << 8U;
  }
  while ((sum >> 16U) != 0) {
    sum = (sum & 0xffffU) + (sum >> 16U);
  }
  return static_cast<std::uint16_t>(~sum & 0xffffU);
}

std::uint16_t checksum_adjust(std::uint16_t checksum, std::uint16_t old_word,
                              std::uint16_t new_word) noexcept {
  const std::uint32_t sum =
      (~std::uint32_t{checksum} & 0xffffU) + (~std::uint32_t{old_word} & 0xffffU) + new_word;
  return static_cast<std::uint16_t>(~fold(sum) & 0xffffU);
}

std::uint8_t decrement_ttl(std::span<std::uint8_t> l3) noexcept {
  std::uint8_t* p = l3.data();
  const std::uint16_t old_word = load_be16(p + kTtlOffset);
  p[kTtlOffset] = static_cast<std::uint8_t>(p[kTtlOffset] - 1U);
  const std::uint16_t new_word = load_be16(p + kTtlOffset);
  store_be16(p + kChecksumOffset,
             checksum_adjust(load_be16(p + kChecksumOffset), old_word, new_word));
  return p[kTtlOffset];
}

std::size_t write_ipv4_header(std::span<std::uint8_t> out, const Ipv4Header& h) noexcept {
  if (out.size() < kIpv4MinHeaderLen) {
    return 0;
  }
  std::uint8_t* p = out.data();
  p[0] = 0x45;
  p[1] = h.tos;
  store_be16(p + 2, h.total_len);
  store_be16(p + 4, h.id);
  store_be16(p + 6, h.flags_frag);
  p[kTtlOffset] = h.ttl;
  p[9] = h.protocol;
  store_be16(p + kChecksumOffset, 0);
  store_be32(p + 12, h.src.value);
  store_be32(p + 16, h.dst.value);
  store_be16(p + kChecksumOffset, internet_checksum(out.first(kIpv4MinHeaderLen)));
  return kIpv4MinHeaderLen;
}

}  // namespace softswitch::pkt
