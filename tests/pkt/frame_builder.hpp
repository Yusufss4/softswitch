#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "softswitch/common/types.hpp"
#include "softswitch/pkt/bytes.hpp"

namespace softswitch::test {

/// Small helper for assembling test frames byte by byte.
class FrameBuilder {
 public:
  FrameBuilder& mac(std::string_view text) {
    const auto m = *MacAddress::parse(text);
    bytes_.insert(bytes_.end(), m.bytes.begin(), m.bytes.end());
    return *this;
  }
  FrameBuilder& u8(std::uint8_t v) {
    bytes_.push_back(v);
    return *this;
  }
  FrameBuilder& u16(std::uint16_t v) {
    bytes_.push_back(static_cast<std::uint8_t>(v >> 8U));
    bytes_.push_back(static_cast<std::uint8_t>(v & 0xffU));
    return *this;
  }
  FrameBuilder& u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v >> 16U));
    return u16(static_cast<std::uint16_t>(v & 0xffffU));
  }
  FrameBuilder& ip(std::string_view text) { return u32(Ipv4Address::parse(text)->value); }
  FrameBuilder& raw(std::initializer_list<std::uint8_t> v) {
    bytes_.insert(bytes_.end(), v);
    return *this;
  }
  FrameBuilder& zeros(std::size_t n) {
    bytes_.insert(bytes_.end(), n, 0);
    return *this;
  }
  /// Patches a 16-bit big-endian value at @p offset.
  FrameBuilder& patch16(std::size_t offset, std::uint16_t v) {
    pkt::store_be16(bytes_.data() + offset, v);
    return *this;
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }
  [[nodiscard]] std::vector<std::uint8_t> build() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace softswitch::test
