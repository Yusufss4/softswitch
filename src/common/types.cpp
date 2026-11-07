#include "softswitch/common/types.hpp"

#include <charconv>
#include <cstring>

namespace softswitch {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::optional<unsigned> hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return static_cast<unsigned>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<unsigned>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<unsigned>(c - 'A' + 10);
  }
  return std::nullopt;
}

std::optional<unsigned> parse_decimal(std::string_view text, unsigned max) noexcept {
  unsigned value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (text.empty() || ec != std::errc{} || ptr != last || value > max) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

MacAddress MacAddress::from_bytes(const std::uint8_t* src) noexcept {
  MacAddress mac;
  std::memcpy(mac.bytes.data(), src, mac.bytes.size());
  return mac;
}

void MacAddress::write_to(std::uint8_t* dst) const noexcept {
  std::memcpy(dst, bytes.data(), bytes.size());
}

std::string MacAddress::to_string() const {
  std::string out;
  out.reserve(17);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      out.push_back(':');
    }
    out.push_back(kHexDigits[bytes[i] >> 4U]);
    out.push_back(kHexDigits[bytes[i] & 0x0fU]);
  }
  return out;
}

std::optional<MacAddress> MacAddress::parse(std::string_view text) {
  if (text.size() != 17) {
    return std::nullopt;
  }
  MacAddress mac;
  for (std::size_t i = 0; i < mac.bytes.size(); ++i) {
    const auto hi = hex_value(text[i * 3]);
    const auto lo = hex_value(text[(i * 3) + 1]);
    if (!hi || !lo) {
      return std::nullopt;
    }
    if (i + 1 < mac.bytes.size() && text[(i * 3) + 2] != ':' && text[(i * 3) + 2] != '-') {
      return std::nullopt;
    }
    mac.bytes[i] = static_cast<std::uint8_t>((*hi << 4U) | *lo);
  }
  return mac;
}

std::string Ipv4Address::to_string() const {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += std::to_string((value >> static_cast<unsigned>(shift)) & 0xffU);
    if (shift != 0) {
      out.push_back('.');
    }
  }
  return out;
}

std::optional<Ipv4Address> Ipv4Address::parse(std::string_view text) {
  std::uint32_t value = 0;
  for (int octet = 0; octet < 4; ++octet) {
    const auto dot = text.find('.');
    const bool last = octet == 3;
    if (last != (dot == std::string_view::npos)) {
      return std::nullopt;
    }
    const auto part = parse_decimal(text.substr(0, dot), 255);
    if (!part) {
      return std::nullopt;
    }
    value = (value << 8U) | *part;
    text = last ? std::string_view{} : text.substr(dot + 1);
  }
  return Ipv4Address{value};
}

std::string Ipv4Prefix::to_string() const {
  return address.to_string() + "/" + std::to_string(length);
}

std::optional<Ipv4Prefix> Ipv4Prefix::parse(std::string_view text) {
  const auto slash = text.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto addr = Ipv4Address::parse(text.substr(0, slash));
  const auto len = parse_decimal(text.substr(slash + 1), 32);
  if (!addr || !len) {
    return std::nullopt;
  }
  return Ipv4Prefix{*addr, static_cast<std::uint8_t>(*len)};
}

}  // namespace softswitch
