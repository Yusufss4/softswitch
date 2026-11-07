# softswitch

[![CI](https://github.com/Yusufss4/softswitch/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Yusufss4/softswitch/actions/workflows/ci.yml)
[![CodeQL](https://github.com/Yusufss4/softswitch/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/Yusufss4/softswitch/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/Yusufss4/softswitch?sort=semver)](https://github.com/Yusufss4/softswitch/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-informational)
![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%205%20%7C%20Linux-lightgrey)

A small user-space L2/L3 Ethernet switch for Linux, written in C++20. It's structured
like the software stack on a Marvell or Broadcom switch: a **control plane** owns
configuration and protocol state, and a separate, pinned, real-time **data plane** forwards
packets using tables the control plane publishes through shared memory.

> **Status:** M4 done: 802.1Q VLANs, MAC learning/aging, Rapid Spanning Tree, IGMP snooping and a CLI on the split control/data plane. See the
> [milestones](https://github.com/Yusufss4/softswitch/milestones) for the roadmap.

## Planned features
- 802.1Q VLANs: tag insert/strip, access/trunk ports, ingress/egress filtering
- MAC learning, flooding and aging (FDB)
- Rapid Spanning Tree (reduced RSTP), verified across three switches
- IGMP snooping, so multicast isn't flooded
- L3: ARP, ICMP, and routing between two VLAN interfaces, plus static routes
- Control plane: CLI and rtnetlink. Data plane: AF_PACKET (TPACKET_V3), SCHED_FIFO, lock-free IPC
- A read-only web GUI that shows live forwarding decisions
- Benchmarks: packets per second, a latency histogram and perf profiles

## Try it

    cmake --preset release && cmake --build --preset release
    scripts/topo.sh up --loop && scripts/switches.sh start   # 3 switches in a loop, 5 hosts
    sudo ip netns exec h1 ping 10.0.10.13                 # h1 -> h3 across two switches
    build/release/sscli -s sw2 show spanning-tree        # one port blocks the loop
    build/release/sscli -s sw1 show mac address-table
    build/release/sscli -s sw1                            # interactive CLI
    scripts/switches.sh stop && scripts/topo.sh down

## Build

Requires CMake ≥ 3.21, Ninja, a C++20 compiler (GCC 12+ / Clang 14+) and GoogleTest.

    cmake --preset dev
    cmake --build --preset dev
    ctest --preset dev

Release packages for Raspberry Pi OS / Debian bookworm (arm64 and amd64) are attached to every
[GitHub release](https://github.com/Yusufss4/softswitch/releases).

## Quality gates
Every pull request runs:

| Check | Tooling |
|---|---|
| Format & hygiene | pre-commit, clang-format 18, `.editorconfig` |
| Build & unit tests | GCC and Clang on x86-64 and arm64, plus Debian bookworm (the Pi's toolchain) |
| Sanitizers | ASan + UBSan, TSan |
| Static analysis | clang-tidy, Cppcheck (HTML report), CodeQL |
| Dynamic analysis | Valgrind memcheck (XML report) |
| Coverage | gcovr: HTML report, with the summary posted on the PR |
| Fuzzing | libFuzzer + ASan/UBSan |
| Performance | Google Benchmark, compared against the main branch's baseline |

See [CONTRIBUTING.md](CONTRIBUTING.md) for running these locally.

## Documentation
- API reference: <https://yusufss4.github.io/softswitch/> (published with each release)
- Design notes: `docs/design.md` (arrives with M1)

## License
[MIT](LICENSE)
