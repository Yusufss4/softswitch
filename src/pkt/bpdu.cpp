#include "softswitch/pkt/bpdu.hpp"

#include "softswitch/pkt/bytes.hpp"
#include "softswitch/pkt/ethernet.hpp"

namespace softswitch::pkt {
namespace {
constexpr std::uint8_t kLlcSapStp = 0x42;
constexpr std::uint8_t kLlcUi = 0x03;
constexpr std::size_t kLlcLen = 3;
constexpr std::size_t kTcnLen = 4;
constexpr std::size_t kConfigLen = 35;
constexpr std::size_t kRstLen = 36;
}  // namespace

std::optional<Bpdu> parse_bpdu_frame(std::span<const std::uint8_t> frame) noexcept {
  const auto eth = parse_ethernet(frame);
  if (!eth || eth->tagged || eth->dst != kStpGroupMac || eth->ethertype >= kEtherTypeMin) {
    return std::nullopt;
  }
  // The 802.3 length covers LLC + BPDU; trailing padding is allowed.
  const std::size_t length = eth->ethertype;
  if (length < kLlcLen + kTcnLen || frame.size() - kEthHeaderLen < length) {
    return std::nullopt;
  }
  const std::uint8_t* llc = frame.data() + kEthHeaderLen;
  if (llc[0] != kLlcSapStp || llc[1] != kLlcSapStp || llc[2] != kLlcUi) {
    return std::nullopt;
  }
  const std::uint8_t* b = llc + kLlcLen;
  const std::size_t bpdu_len = length - kLlcLen;
  if (load_be16(b) != 0) {  // protocol identifier
    return std::nullopt;
  }
  Bpdu bpdu;
  bpdu.version = b[2];
  bpdu.type = b[3];
  if (bpdu.type == kBpduTypeTcn) {
    return bpdu;
  }
  const std::size_t need = bpdu.type == kBpduTypeRst ? kRstLen : kConfigLen;
  if ((bpdu.type != kBpduTypeConfig && bpdu.type != kBpduTypeRst) || bpdu_len < need) {
    return std::nullopt;
  }
  bpdu.flags = b[4];
  bpdu.root_id = load_be64(b + 5);
  bpdu.root_path_cost = load_be32(b + 13);
  bpdu.bridge_id = load_be64(b + 17);
  bpdu.port_id = load_be16(b + 25);
  bpdu.message_age = load_be16(b + 27);
  bpdu.max_age = load_be16(b + 29);
  bpdu.hello_time = load_be16(b + 31);
  bpdu.forward_delay = load_be16(b + 33);
  return bpdu;
}

std::size_t build_bpdu_frame(std::span<std::uint8_t> out, const MacAddress& src,
                             const Bpdu& bpdu) noexcept {
  const bool tcn = bpdu.type == kBpduTypeTcn;
  const std::size_t bpdu_len = tcn ? kTcnLen : (bpdu.type == kBpduTypeRst ? kRstLen : kConfigLen);
  if (out.size() < kMinFrameLen || out.size() < kEthHeaderLen + kLlcLen + bpdu_len) {
    return 0;
  }
  write_eth_header(out, kStpGroupMac, src, static_cast<std::uint16_t>(kLlcLen + bpdu_len));
  std::uint8_t* llc = out.data() + kEthHeaderLen;
  llc[0] = kLlcSapStp;
  llc[1] = kLlcSapStp;
  llc[2] = kLlcUi;
  std::uint8_t* b = llc + kLlcLen;
  store_be16(b, 0);
  b[2] = bpdu.version;
  b[3] = bpdu.type;
  if (!tcn) {
    b[4] = bpdu.flags;
    store_be64(b + 5, bpdu.root_id);
    store_be32(b + 13, bpdu.root_path_cost);
    store_be64(b + 17, bpdu.bridge_id);
    store_be16(b + 25, bpdu.port_id);
    store_be16(b + 27, bpdu.message_age);
    store_be16(b + 29, bpdu.max_age);
    store_be16(b + 31, bpdu.hello_time);
    store_be16(b + 33, bpdu.forward_delay);
    if (bpdu.type == kBpduTypeRst) {
      b[35] = 0;  // version 1 length
    }
  }
  return pad_frame(out, kEthHeaderLen + kLlcLen + bpdu_len);
}

}  // namespace softswitch::pkt
