find_package(benchmark REQUIRED)

add_executable(softswitch_bench
  bench_fdb.cpp
  bench_pkt.cpp
  bench_shm.cpp
)
target_link_libraries(softswitch_bench PRIVATE softswitch::proto benchmark::benchmark_main)
