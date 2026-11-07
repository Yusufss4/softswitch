// Feeds arbitrary bytes to every packet parser and checks transform invariants:
//  - parsers never read out of bounds (ASan) and never crash,
//  - insert_tag followed by strip_tag restores the original frame,
//  - copy_with_tag agrees with insert_tag,
//  - an RST BPDU that parses survives a build/parse round trip,
//  - echo_request_to_reply produces valid checksums.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "softswitch/pkt/arp.hpp"
#include "softswitch/pkt/bpdu.hpp"
#include "softswitch/pkt/ethernet.hpp"
#include "softswitch/pkt/icmp.hpp"
#include "softswitch/pkt/igmp.hpp"
#include "softswitch/pkt/ipv4.hpp"

using namespace softswitch::pkt;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);

  (void)parse_arp(in);
  (void)parse_ipv4(in);
  (void)ipv4_header_ok(in);
  (void)parse_icmp(in);
  (void)parse_igmp(in);
  (void)parse_bpdu_frame(in);

  if (const auto eth = parse_ethernet(in)) {
    const auto payload = in.subspan(eth->payload_offset);
    (void)parse_arp(payload);
    if (const auto ip = parse_ipv4(payload)) {
      const auto l4 = payload.subspan(ip->header_len, ip->total_len - ip->header_len);
      (void)parse_icmp(l4);
      (void)parse_igmp(l4);
    }
  }

  if (size >= kEthHeaderLen && size <= kMaxFrameLen) {
    std::vector<std::uint8_t> buf(in.begin(), in.end());
    buf.resize(size + kVlanTagLen);
    if (insert_tag(buf, size, make_tci(123, 4)) != size + kVlanTagLen) std::abort();
    std::vector<std::uint8_t> copied(size + kVlanTagLen);
    if (copy_with_tag(in, make_tci(123, 4), copied) != size + kVlanTagLen) std::abort();
    if (copied != buf) std::abort();
    if (strip_tag(buf) != size) std::abort();
    if (std::memcmp(buf.data(), data, size) != 0) std::abort();

    std::vector<std::uint8_t> csum(in.begin(), in.end());
    if (complete_l4_checksum(csum)) {
      // Completing an already complete checksum must not change it.
      auto twice = csum;
      (void)complete_l4_checksum(twice);
      if (twice != csum) std::abort();
    }

    std::vector<std::uint8_t> echo(in.begin(), in.end());
    if (echo_request_to_reply(echo)) {
      const auto l3 = std::span{echo}.subspan(kEthHeaderLen);
      if (!ipv4_header_ok(l3)) std::abort();
    }
  }

  if (const auto bpdu = parse_bpdu_frame(in); bpdu && bpdu->type == kBpduTypeRst) {
    std::array<std::uint8_t, 64> out{};
    const auto len = build_bpdu_frame(out, softswitch::MacAddress{}, *bpdu);
    const auto again = parse_bpdu_frame(std::span{out}.first(len));
    if (!again || again->root_id != bpdu->root_id || again->flags != bpdu->flags ||
        again->port_id != bpdu->port_id || again->forward_delay != bpdu->forward_delay) {
      std::abort();
    }
  }
  return 0;
}
