// Feeds arbitrary frames (with arbitrary offloaded-tag metadata and ingress port) through the
// forwarding pipeline over a fixed VLAN configuration and checks the 802.1Q invariants:
//  - nothing is ever sent back out of the ingress port,
//  - nothing leaves on a port that isn't a member of the frame's VLAN,
//  - frames leave untagged exactly on the VLAN's untagged ports,
//  - emitted frames are well-formed and never carry an in-band tag of their own.
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>

#include "softswitch/dp/pipeline.hpp"
#include "softswitch/pkt/ethernet.hpp"

using namespace softswitch;

namespace {

struct CheckingEgress final : dp::Egress {
  const shm::Tables* tables = nullptr;
  PortId in_port = 0;
  void emit(PortId port, std::span<const std::uint8_t> untagged,
            std::optional<std::uint16_t> tci) noexcept override {
    if (port == in_port || port >= tables->num_ports) std::abort();
    const auto eth = pkt::parse_ethernet(untagged);
    if (!eth || eth->tagged) std::abort();
    // Which VLAN is this? Tagged egress says so; untagged egress must be the port's PVID.
    const VlanId vlan = tci ? pkt::tci_vid(*tci) : tables->ports[port].pvid;
    const auto& entry = tables->vlans[vlan];
    if (!has_port(entry.members, port)) std::abort();
    if (tci.has_value() == has_port(entry.untagged, port)) std::abort();
  }
};

struct World {
  std::unique_ptr<shm::SharedRegion> region = std::make_unique<shm::SharedRegion>();
  shm::Tables& t = region->tables[0];
  World() {
    t.num_ports = 4;
    t.trace_sample = 3;
    for (PortId p = 0; p < 4; ++p) {
      t.ports[p].flags = shm::port_flag::kAdminUp | shm::port_flag::kOperUp;
    }
    for (VlanId v : {VlanId{10}, VlanId{20}}) {
      t.vlans[v].flags = shm::vlan_flag::kActive;
    }
    // p0 access 10, p1 access 20, p2 trunk {10,20} native 10, p3 trunk {20} no native.
    t.ports[0].pvid = 10;
    t.ports[1].pvid = 20;
    t.ports[2].mode = shm::PortMode::kTrunk;
    t.ports[2].pvid = 10;
    t.ports[3].mode = shm::PortMode::kTrunk;
    t.ports[3].pvid = 0;
    t.vlans[10].members = 0b0101;
    t.vlans[10].untagged = 0b0101;
    t.vlans[20].members = 0b1110;
    t.vlans[20].untagged = 0b0010;
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static World world;
  if (size < 3) {
    return 0;
  }
  dp::Pipeline pipeline(*world.region);
  CheckingEgress egress;
  egress.tables = &world.t;
  egress.in_port = static_cast<PortId>(data[0] % 5);  // includes an out-of-range port
  dp::RxFrame frame;
  frame.vlan_tci_valid = (data[1] & 1U) != 0;
  frame.checksum_incomplete = (data[1] & 2U) != 0;
  frame.vlan_tci = static_cast<std::uint16_t>((data[1] << 8U) | data[2]);
  frame.data = std::span<const std::uint8_t>(data + 3, size - 3);
  pipeline.process(world.t, egress.in_port, frame, egress);
  shm::TraceRecord rec;
  while (world.region->trace.try_pop(rec)) {
  }
  return 0;
}
