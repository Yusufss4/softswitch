# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
