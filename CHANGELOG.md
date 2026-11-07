# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

### Removed
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

[Unreleased]: https://github.com/Yusufss4/softswitch/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Yusufss4/softswitch/releases/tag/v0.1.0
