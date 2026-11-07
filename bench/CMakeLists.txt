find_package(benchmark REQUIRED)

add_executable(softswitch_bench
  bench_mac_table.cpp
  bench_pkt.cpp
)
target_link_libraries(softswitch_bench PRIVATE softswitch::proto benchmark::benchmark_main)
