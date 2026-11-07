find_package(GTest REQUIRED)
include(GoogleTest)

# Pure-logic unit tests (no privileges, no network).
add_executable(softswitch_tests
  test_mac_table.cpp
  test_types.cpp
  pkt/test_ethernet.cpp
  pkt/test_igmp_bpdu.cpp
  pkt/test_ipv4_icmp_arp.cpp
)
target_include_directories(softswitch_tests PRIVATE pkt)
target_link_libraries(softswitch_tests PRIVATE softswitch::proto GTest::gtest_main)
gtest_discover_tests(softswitch_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
