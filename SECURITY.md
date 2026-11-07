# Security policy

softswitch parses untrusted network traffic, so memory-safety bugs in the data plane are
security bugs.

## Supported versions
Only the latest release gets fixes.

## Reporting a vulnerability
Please **don't open a public issue**. Report it privately through
[GitHub security advisories](https://github.com/Yusufss4/softswitch/security/advisories/new)
and include a packet capture or fuzzer reproducer if you have one. You'll get an
acknowledgement within 7 days.

## Hardening
- Every packet parser has a libFuzzer target that runs in CI under ASan/UBSan.
- CodeQL, clang-tidy and Cppcheck run on every pull request.
- The daemons run with file capabilities (`cap_net_raw`, `cap_net_admin`, `cap_sys_nice`,
  `cap_ipc_lock`) instead of as root. The web GUI is read-only.
