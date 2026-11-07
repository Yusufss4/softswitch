#pragma once

/// @file
/// Big-endian (network order) loads and stores on raw packet bytes.

#include <cstdint>

namespace softswitch::pkt {

[[nodiscard]] constexpr std::uint16_t load_be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((std::uint32_t{p[0]} << 8U) | std::uint32_t{p[1]});
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) | (std::uint32_t{p[2]} << 8U) |
         std::uint32_t{p[3]};
}

[[nodiscard]] constexpr std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  return (std::uint64_t{load_be32(p)} << 32U) | load_be32(p + 4);
}

constexpr void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 8U);
  p[1] = static_cast<std::uint8_t>(v & 0xffU);
}

constexpr void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24U);
  p[1] = static_cast<std::uint8_t>((v >> 16U) & 0xffU);
  p[2] = static_cast<std::uint8_t>((v >> 8U) & 0xffU);
  p[3] = static_cast<std::uint8_t>(v & 0xffU);
}

constexpr void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  store_be32(p, static_cast<std::uint32_t>(v >> 32U));
  store_be32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFU));
}

}  // namespace softswitch::pkt
