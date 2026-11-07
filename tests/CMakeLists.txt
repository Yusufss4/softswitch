find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(softswitch_tests
  test_mac_table.cpp
)
target_link_libraries(softswitch_tests PRIVATE softswitch::core GTest::gtest_main)
gtest_discover_tests(softswitch_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
