#pragma once

/// @file
/// Basic switch vocabulary types: ports, VLANs, MAC and IPv4 addresses.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace softswitch {

/// Logical switch port index (0-based).
using PortId = std::uint16_t;
/// 802.1Q VLAN identifier.
using VlanId = std::uint16_t;
/// Set of ports; bit N stands for port N.
using PortMask = std::uint32_t;

inline constexpr PortId kMaxPorts = 32;
inline constexpr PortId kInvalidPort = 0xFFFF;
inline constexpr VlanId kMinVlanId = 1;
inline constexpr VlanId kMaxVlanId = 4094;
inline constexpr std::size_t kVlanTableSize = 4096;

[[nodiscard]] constexpr PortMask port_bit(PortId port) noexcept {
  return port < kMaxPorts ? PortMask{1} << port : PortMask{0};
}

[[nodiscard]] constexpr bool has_port(PortMask mask, PortId port) noexcept {
  return (mask & port_bit(port)) != 0;
}

[[nodiscard]] constexpr bool valid_vlan(unsigned vid) noexcept {
  return vid >= kMinVlanId && vid <= kMaxVlanId;
}

/// 64-bit finalizer (splitmix64); good avalanche for table hashing.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 30U;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27U;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return x;
}

/// 48-bit Ethernet address in transmission order.
struct MacAddress {
  std::array<std::uint8_t, 6> bytes{};

  [[nodiscard]] static constexpr MacAddress broadcast() noexcept {
    return MacAddress{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
  }
  [[nodiscard]] constexpr bool is_multicast() const noexcept { return (bytes[0] & 0x01U) != 0; }
  [[nodiscard]] constexpr bool is_broadcast() const noexcept { return *this == broadcast(); }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return *this == MacAddress{}; }

  [[nodiscard]] constexpr std::uint64_t to_u64() const noexcept {
    std::uint64_t value = 0;
    for (const std::uint8_t b : bytes) {
      value = (value << 8U) | b;
    }
    return value;
  }
  [[nodiscard]] static constexpr MacAddress from_u64(std::uint64_t value) noexcept {
    MacAddress mac;
    for (std::size_t i = mac.bytes.size(); i-- > 0;) {
      mac.bytes[i] = static_cast<std::uint8_t>(value & 0xffU);
      value >>= 8U;
    }
    return mac;
  }

  /// Reads 6 bytes from @p src.
  [[nodiscard]] static MacAddress from_bytes(const std::uint8_t* src) noexcept;
  /// Writes 6 bytes to @p dst.
  void write_to(std::uint8_t* dst) const noexcept;

  /// "aa:bb:cc:dd:ee:ff"
  [[nodiscard]] std::string to_string() const;
  /// Accepts "aa:bb:cc:dd:ee:ff" or "aa-bb-cc-dd-ee-ff".
  [[nodiscard]] static std::optional<MacAddress> parse(std::string_view text);

  friend constexpr bool operator==(const MacAddress&, const MacAddress&) = default;
  friend constexpr auto operator<=>(const MacAddress&, const MacAddress&) = default;
};

/// IPv4 address, stored in host byte order.
struct Ipv4Address {
  std::uint32_t value{};

  [[nodiscard]] static constexpr Ipv4Address from_octets(std::uint8_t a, std::uint8_t b,
                                                         std::uint8_t c, std::uint8_t d) noexcept {
    return Ipv4Address{(std::uint32_t{a} << 24U) | (std::uint32_t{b} << 16U) |
                       (std::uint32_t{c} << 8U) | std::uint32_t{d}};
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value == 0; }
  [[nodiscard]] constexpr bool is_multicast() const noexcept { return (value >> 28U) == 0xEU; }
  /// 224.0.0.0/24: control traffic that snooping switches always flood.
  [[nodiscard]] constexpr bool is_local_multicast() const noexcept {
    return (value & 0xFFFFFF00U) == 0xE0000000U;
  }
  [[nodiscard]] constexpr bool is_broadcast() const noexcept { return value == 0xFFFFFFFFU; }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static std::optional<Ipv4Address> parse(std::string_view text);

  friend constexpr bool operator==(const Ipv4Address&, const Ipv4Address&) = default;
  friend constexpr auto operator<=>(const Ipv4Address&, const Ipv4Address&) = default;
};

[[nodiscard]] constexpr std::uint32_t prefix_mask(unsigned length) noexcept {
  if (length == 0) {
    return 0;
  }
  return length >= 32 ? 0xFFFFFFFFU : 0xFFFFFFFFU << (32U - length);
}

/// IPv4 prefix such as 10.0.10.0/24 (the host bits of `address` are kept as given).
struct Ipv4Prefix {
  Ipv4Address address;
  std::uint8_t length = 0;

  [[nodiscard]] constexpr Ipv4Address network() const noexcept {
    return Ipv4Address{address.value & prefix_mask(length)};
  }
  [[nodiscard]] constexpr bool contains(Ipv4Address ip) const noexcept {
    return ((ip.value ^ address.value) & prefix_mask(length)) == 0;
  }

  [[nodiscard]] std::string to_string() const;
  /// Accepts "a.b.c.d/len".
  [[nodiscard]] static std::optional<Ipv4Prefix> parse(std::string_view text);

  friend constexpr bool operator==(const Ipv4Prefix&, const Ipv4Prefix&) = default;
};

/// Ethernet address an IPv4 multicast group maps to (RFC 1112): 01:00:5e + low 23 bits.
[[nodiscard]] constexpr MacAddress multicast_mac(Ipv4Address group) noexcept {
  return MacAddress{{0x01, 0x00, 0x5e, static_cast<std::uint8_t>((group.value >> 16U) & 0x7fU),
                     static_cast<std::uint8_t>((group.value >> 8U) & 0xffU),
                     static_cast<std::uint8_t>(group.value & 0xffU)}};
}

}  // namespace softswitch
