# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Reduced Rapid Spanning Tree state machine (`stp::Rstp`): priority-vector role selection,
  proposal/agreement with sync, instant alternate-to-root failover, forward-delay fallback,
  received-information expiry, edge ports and topology-change flushing/propagation. Agreements
  from root ports are only accepted from bridges that are really downstream, which closes the
  forwarding loop RSTP's count-to-infinity can otherwise open after a root bridge is lost.
- RSTP simulation tests (triangle convergence, failover, silent failure, root loss, priority
  change, edge, fallback, backup port) and a property test over 500 random meshes asserting a
  loop-free spanning tree after convergence and after link failures; `fuzz_rstp` fuzz target.
- rtnetlink link monitoring in `softswitchd`: an initial RTM_GETLINK dump plus RTMGRP_LINK
  events set each port's operational state (admin up and carrier); a port going down flushes the
  MACs learned on it. Re-dumps after a netlink overrun. Bounds-checked parser with tests and a
  `fuzz_netlink` target.

## [0.3.0] - 2026-11-07

### Added
- CLI: switch configuration model (VLANs, access/trunk ports, native VLAN, allowed VLAN lists,
  descriptions, shutdown, MAC aging time, trace sampling) with Cisco-style one-line commands.
- `show version | running-config | interfaces | vlan | counters | dataplane`; `show
  running-config` output replays as a startup config (round-trip tested and fuzzed).
- CLI Unix socket in `softswitchd` and the `sscli` client (one-shot or interactive);
  `softswitchd --config FILE` loads a startup config.
- Startup configs for the test topology (`configs/sw{1,2,3}.conf`): VLAN 10/20 with 802.1Q trunks.
- `fuzz_config` fuzz target with a seed corpus under `fuzz/corpus/config`.
- 802.1Q VLAN enforcement in the data plane: tagged frames dropped on access ports
  (priority-tagged accepted), untagged frames classified to the PVID or dropped when the port has
  none, ingress filtering on VLAN membership, egress only to members with per-port tagging, and
  IEEE reserved group addresses (01:80:C2:00:00:0x) never forwarded. Drops are counted per reason.
- `fuzz_pipeline` fuzz target asserting the VLAN forwarding invariants on arbitrary frames.
- MAC learning and aging: the control plane owns the FDB (`l2::Fdb`, keyed by MAC+VLAN); the data
  plane reports new and moved source MACs on the learn ring (with 10 ms repeat suppression) and
  marks hits in a hash-indexed bitmap the control plane harvests every second for aging.
- Known unicast is forwarded to a single port; frames to the ingress port are filtered; unknown
  unicast, broadcast and multicast are flooded within the VLAN.
- `show mac address-table [vlan ID] [interface PORT]`, `clear mac address-table`,
  `mac address-table static MAC vlan ID interface PORT`; entries invalidated by configuration
  (port shut down, VLAN pruned) are flushed.
- STP port state honoured on ingress: discarding ports neither learn nor forward, learning ports
  only learn.
- `fuzz_fdb` fuzz target and FDB learn/publish benchmarks.

### Removed
- `MacTable` (replaced by `l2::Fdb`).

## [0.2.0] - 2026-11-07

### Added
- Packet library (`softswitch::pkt`): bounds-checked parsers and builders for Ethernet/802.1Q
  (tag insert/strip in place and on copy), ARP, IPv4 (RFC 1071 checksum, RFC 1624 incremental
  TTL update), ICMP echo/error, IGMP v1/v2/v3 and spanning-tree BPDUs (config, TCN, RST).
- Common vocabulary types: `MacAddress`, `Ipv4Address`, `Ipv4Prefix`, port masks.
- `fuzz_packet` libFuzzer target, packet micro-benchmarks, `scripts/check.sh` (local CI gates).
- Shared-memory IPC (`softswitch::shm`): fixed-capacity forwarding tables (FDB, MDB, neighbors,
  routes, VLANs, ports), lock-free SPSC rings for learn/punt/inject/trace messages, and
  double-buffered table publication with a quiescent-state grace period, so the data plane never
  waits for the control plane.
- System wrappers (`softswitch::sys`): RAII descriptors, eventfd, sealed memfd shared memory,
  Unix sockets with SCM_RIGHTS descriptor passing.
- `softswitchd` (control plane): epoll event loop with timerfd/signalfd, creates the shared
  region, publishes tables and hands the region to the data plane over a SEQPACKET socket.
- `ss-dataplane` (data plane): attaches over the handshake, opens one AF_PACKET port per switch
  port (TPACKET_V2 RX/TX rings, promiscuous, qdisc bypass) and forwards on a pinned SCHED_FIFO
  thread with `mlockall`. Flood-only forwarding for now.

- netns integration tests: `scripts/topo.sh` (3 switches, 5 hosts), `scripts/switches.sh`,
  `tests/integration/` registered in ctest (`ctest --preset integration`) and run in CI.
- shellcheck in pre-commit.

### Removed
- `scripts/setup_netns.sh` / `teardown_netns.sh` (superseded by `topo.sh`).
- The placeholder `softswitch` executable (replaced by `softswitchd` and `ss-dataplane`).

### Changed
- Release dry runs (`workflow_dispatch`) now run the publish job too (download, SBOM, checksums, notes) and upload the bundle as an artifact instead of creating a release.
- GitHub Actions bumped: upload-artifact v7, download-artifact v8, upload-pages-artifact v5, codeql-action v4, sticky-pull-request-comment v3.

### Fixed
- PR-title lint accepts capitalized subjects (Dependabot's `ci: Bump …`); only a trailing period is rejected.

## [0.1.0] - 2026-11-07

### Added
- Project infrastructure: CMake presets, CPack (`.deb`/`.tar.gz`), sanitizer, coverage and fuzz builds.
- CI pipeline: pre-commit/format, gcc and clang builds on x86-64 and arm64, Debian bookworm (Pi)
  compatibility, ASan/UBSan, TSan, clang-tidy, Cppcheck and Valgrind reports, coverage, libFuzzer,
  Google Benchmark regression tracking, CodeQL, and PR-title linting.
- Release pipeline: arm64 packages, SHA256SUMS, SPDX SBOM, Doxygen docs on GitHub Pages.
- `MacTable`: a MAC learning table with aging (starter L2 forwarding database).

[Unreleased]: https://github.com/Yusufss4/softswitch/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Yusufss4/softswitch/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Yusufss4/softswitch/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Yusufss4/softswitch/releases/tag/v0.1.0
