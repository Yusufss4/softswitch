#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "softswitch/dp/frame.hpp"
#include "softswitch/dp/pipeline.hpp"
#include "softswitch/shm/region.hpp"

namespace softswitch::test {

/// Records every frame the pipeline emits.
class RecordingEgress final : public dp::Egress {
 public:
  struct Sent {
    PortId port;
    std::vector<std::uint8_t> frame;
    std::optional<std::uint16_t> tci;
  };
  void emit(PortId port, std::span<const std::uint8_t> untagged,
            std::optional<std::uint16_t> tci) noexcept override {
    sent.push_back({port, {untagged.begin(), untagged.end()}, tci});
  }
  [[nodiscard]] PortMask ports() const {
    PortMask m = 0;
    for (const auto& s : sent) {
      m |= port_bit(s.port);
    }
    return m;
  }
  std::vector<Sent> sent;
};

/// A region + pipeline with hand-built tables, for exercising forwarding decisions.
struct PipelineHarness {
  std::unique_ptr<shm::SharedRegion> region = std::make_unique<shm::SharedRegion>();
  shm::Tables& tables = region->tables[0];
  dp::Pipeline pipeline{*region};
  RecordingEgress egress;

  explicit PipelineHarness(std::uint32_t num_ports = 4) {
    tables.num_ports = num_ports;
    for (std::uint32_t p = 0; p < num_ports; ++p) {
      tables.ports[p].flags = shm::port_flag::kAdminUp | shm::port_flag::kOperUp;
      tables.ports[p].pvid = 1;
      tables.vlans[1].members |= port_bit(static_cast<PortId>(p));
      tables.vlans[1].untagged |= port_bit(static_cast<PortId>(p));
    }
    tables.vlans[1].flags = shm::vlan_flag::kActive;
  }

  void receive(PortId port, const std::vector<std::uint8_t>& frame,
               std::optional<std::uint16_t> offloaded_tci = std::nullopt) {
    dp::RxFrame rx;
    rx.data = frame;
    rx.vlan_tci_valid = offloaded_tci.has_value();
    rx.vlan_tci = offloaded_tci.value_or(0);
    pipeline.process(tables, port, rx, egress);
  }

  [[nodiscard]] std::uint64_t drops(shm::DropReason reason) const {
    return region->dp.drops[static_cast<std::size_t>(reason)].load();
  }
};

}  // namespace softswitch::test
