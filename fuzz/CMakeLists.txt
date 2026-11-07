if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "SOFTSWITCH_BUILD_FUZZERS requires clang (libFuzzer)")
endif()

# Each fuzz_<name>.cpp becomes a libFuzzer binary built with ASan + UBSan.
function(softswitch_add_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE softswitch::proto)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

softswitch_add_fuzzer(fuzz_packet)
softswitch_add_fuzzer(fuzz_config)
softswitch_add_fuzzer(fuzz_fdb)
softswitch_add_fuzzer(fuzz_pipeline)
softswitch_add_fuzzer(fuzz_rstp)
