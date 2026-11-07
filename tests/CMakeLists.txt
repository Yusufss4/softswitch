find_package(GTest REQUIRED)
include(GoogleTest)

# Pure-logic unit tests (no privileges, no network).
add_executable(softswitch_tests
  test_mac_table.cpp
  test_types.cpp
  cp/test_config.cpp
  dp/test_pipeline_flood.cpp
  pkt/test_ethernet.cpp
  pkt/test_igmp_bpdu.cpp
  pkt/test_ipv4_icmp_arp.cpp
  shm/test_publisher.cpp
  shm/test_spsc_ring.cpp
  shm/test_tables.cpp
)
target_include_directories(softswitch_tests PRIVATE pkt dp)
target_link_libraries(softswitch_tests PRIVATE softswitch::proto GTest::gtest_main)
gtest_discover_tests(softswitch_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# Tests that make syscalls (memfd, sockets, fork) but need no privileges.
add_executable(softswitch_runtime_tests
  shm/test_region_ipc.cpp
)
target_link_libraries(softswitch_runtime_tests PRIVATE softswitch::runtime GTest::gtest_main)
gtest_discover_tests(softswitch_runtime_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# Integration tests: real processes on a veth/netns topology. They need passwordless sudo and
# are excluded from the default test presets; run them with `ctest --preset integration`.
file(GLOB _integration_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_*.sh)
foreach(_script IN LISTS _integration_tests)
  get_filename_component(_name ${_script} NAME_WE)
  add_test(NAME integration.${_name} COMMAND ${_script})
  set_tests_properties(integration.${_name} PROPERTIES
    LABELS integration
    RUN_SERIAL TRUE
    TIMEOUT 180
    ENVIRONMENT "SOFTSWITCH_BIN_DIR=${CMAKE_BINARY_DIR}")
endforeach()
