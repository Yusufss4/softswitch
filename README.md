# softswitch

A user-space L2/L3 software switch in C++20, running on a Raspberry Pi 5.

## Build

    cmake -S . -B build -G Ninja
    cmake --build build
    ctest --test-dir build --output-on-failure

## Test topology

    scripts/setup_netns.sh      # creates hosts h1..h3 wired to ports sw-p1..sw-p3
    scripts/teardown_netns.sh

Don't attach the switch to `eth0`; that's the SSH link.
