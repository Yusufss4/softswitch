# Contributing

## Setup (Debian 12 / Raspberry Pi OS bookworm)

    sudo apt install build-essential cmake ninja-build clang clang-tidy pkg-config \
        libgtest-dev libgmock-dev libbenchmark-dev libpcap-dev libnl-3-dev libnl-route-3-dev \
        cppcheck valgrind gcovr doxygen graphviz pipx
    pipx install pre-commit      # Debian's 3.0 is too old for the pinned clang-format hook
    pre-commit install

## Build and test

| Preset | Purpose |
|---|---|
| `dev` | Debug build with warnings as errors, tests and benchmarks |
| `release` | Optimized build (RelWithDebInfo) |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (x86-64 only: the Pi kernel's 47-bit VMA isn't supported by TSan, so CI covers it) |
| `coverage` | gcc `--coverage`, used by `scripts/coverage.sh` |
| `fuzz` | clang + libFuzzer targets in `fuzz/` |

    cmake --preset dev && cmake --build --preset dev && ctest --preset dev

## Static and dynamic analysis

    CLANG_TIDY=clang-tidy-18 scripts/clang-tidy.sh build/dev   # same version as CI (apt.llvm.org on bookworm)
    scripts/cppcheck.sh build/dev      # report in reports/cppcheck/
    scripts/valgrind.sh build/dev      # memcheck on all unit tests, reports in reports/valgrind/
    scripts/coverage.sh                # report in reports/coverage/

## Pull requests
1. Open or pick an issue. Branch from `main` as `type/area-topic`.
2. Use Conventional Commits, e.g. `feat(stp): add proposal/agreement handshake`.
3. Add tests: unit tests for logic, a fuzz target for anything that parses packets, a benchmark
   for anything on the data-plane hot path.
4. Add an entry under `## [Unreleased]` in `CHANGELOG.md`.
5. Open the PR with the template filled in. It gets merged (squash) once `ci-ok` passes.
