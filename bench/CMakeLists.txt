find_package(benchmark REQUIRED)

add_executable(softswitch_bench
  bench_mac_table.cpp
)
target_link_libraries(softswitch_bench PRIVATE softswitch::core benchmark::benchmark_main)
