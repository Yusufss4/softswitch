cmake_minimum_required(VERSION 3.21)
project(softswitch
  VERSION 0.1.0
  DESCRIPTION "User-space L2/L3 software switch with a split control plane and data plane"
  HOMEPAGE_URL "https://github.com/Yusufss4/softswitch"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(SOFTSWITCH_BUILD_TESTS      "Build unit tests"                         ON)
option(SOFTSWITCH_BUILD_BENCHMARKS "Build Google Benchmark micro-benchmarks"  OFF)
option(SOFTSWITCH_BUILD_FUZZERS    "Build libFuzzer targets (clang only)"     OFF)
option(SOFTSWITCH_WERROR           "Treat compiler warnings as errors"        OFF)
option(SOFTSWITCH_COVERAGE         "Instrument for code coverage (gcc/clang)" OFF)
set(SOFTSWITCH_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address;undefined or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SoftswitchOptions)   # defines softswitch::options (warnings, sanitizers, coverage)

configure_file(include/softswitch/version.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/softswitch/version.hpp" @ONLY)

# ---- libraries ---------------------------------------------------------------
# softswitch::proto: pure protocol logic (no syscalls); unit-tested and fuzzed.
add_library(softswitch_proto
  src/common/types.cpp
  src/mac_table.cpp
  src/pkt/arp.cpp
  src/pkt/bpdu.cpp
  src/pkt/ethernet.cpp
  src/pkt/icmp.cpp
  src/pkt/igmp.cpp
  src/pkt/ipv4.cpp
)
add_library(softswitch::proto ALIAS softswitch_proto)
target_include_directories(softswitch_proto PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(softswitch_proto PUBLIC softswitch::options)

# ---- executables ------------------------------------------------------------
add_executable(softswitch src/main.cpp)
target_link_libraries(softswitch PRIVATE softswitch::proto)

# ---- tests / benchmarks / fuzzers -------------------------------------------
if(SOFTSWITCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(SOFTSWITCH_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
if(SOFTSWITCH_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---- docs -------------------------------------------------------------------
find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
  configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
  add_custom_target(docs
    COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM)
endif()

# ---- install / package ------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS softswitch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "softswitch")
set(CPACK_PACKAGE_VENDOR "Yusuf Savaş")
set(CPACK_PACKAGE_CONTACT "Yusuf Savaş <yusufsss4@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")
include(CPack)
