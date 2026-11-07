# softswitch: rules for working in this repo

A user-space L2/L3 switch in C++20, split into a control plane and a data plane the way
Marvell/Broadcom switch software is split. Target: Raspberry Pi 5 (Debian 12 bookworm, arm64).

## Where things run
- Code lives on the Pi at `~/git/softswitch` (`ssh pi@192.168.1.169`). Build, test and run there.
- **Never attach the switch to `eth0`.** It carries SSH. Use the veth/netns topology in `scripts/`.

## Git workflow
- `main` is protected. Never push to it directly; every change lands through a PR.
- Branches: `feat/<area>-<topic>`, `fix/...`, `perf/...`, `test/...`, `docs/...`, `ci/...`, `chore/...`.
- Commits follow [Conventional Commits](https://www.conventionalcommits.org/):
  `type(scope): summary`. Scopes: `pkt shm l2 stp igmp l3 dp cp gui cli bench ci docs build`.
  Keep commits small. Each one builds and passes its tests.
- PR titles use the same format; CI lints them. A PR is one feature, links its issue
  (`Closes #N`), fills in the template and includes the real test output.
- Merge only when `ci-ok` passes. Squash-merge; the branch gets deleted.

## Releases
- SemVer. Each milestone ends with a release: M0 → v0.1.0 … M6 → v0.7.0, M7 (docs + benchmarks) → v1.0.0.
- Update `CHANGELOG.md` (Keep a Changelog) in the same PR as the change, under `## [Unreleased]`.
- To release: open a `chore(release): vX.Y.Z` PR that moves Unreleased into a version section and
  bumps `project(VERSION)` in `CMakeLists.txt`. After merging it, tag `vX.Y.Z` on main and push the tag.
  The release workflow builds the bookworm/arm64 `.deb` and `.tar.gz`, plus SHA256SUMS, an SBOM and the docs.

## Code rules
- C++20, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, and CI builds with `-Werror`.
- Format with `pre-commit run --all-files` (pinned clang-format). `clang-tidy` and `cppcheck` must be clean.
  Any suppression needs a comment explaining why.
- Data-plane hot path: no allocation, no locks, no blocking syscalls, no logging. It reads
  control-plane tables only through the published snapshot. See `docs/design.md`.
- Protocol logic (VLAN, FDB, RSTP, IGMP, L3) is written as pure state machines with an injected
  clock, and each one gets GoogleTest tests. Packet parsers get a fuzz target in `fuzz/`.
- Performance-sensitive code gets a Google Benchmark in `bench/`.

## Commands
    cmake --preset dev && cmake --build --preset dev && ctest --preset dev
    scripts/clang-tidy.sh build/dev     scripts/cppcheck.sh build/dev
    scripts/valgrind.sh build/dev       scripts/coverage.sh
