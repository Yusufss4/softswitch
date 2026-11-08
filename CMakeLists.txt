cmake_minimum_required(VERSION 3.21)
project(softswitch
  VERSION 0.6.0
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
  src/common/log.cpp
  src/common/types.cpp
  src/cp/config.cpp
  src/cp/netlink_parse.cpp
  src/cp/text_table.cpp
  src/dp/pipeline.cpp
  src/gui/http_parse.cpp
  src/gui/json.cpp
  src/igmp/snooper.cpp
  src/l2/fdb.cpp
  src/l3/router.cpp
  src/stp/rstp.cpp
  src/pkt/arp.cpp
  src/pkt/bpdu.cpp
  src/pkt/ethernet.cpp
  src/pkt/icmp.cpp
  src/pkt/igmp.cpp
  src/pkt/ipv4.cpp
  src/shm/names.cpp
  src/shm/publisher.cpp
  src/shm/tables.cpp
)
add_library(softswitch::proto ALIAS softswitch_proto)
target_include_directories(softswitch_proto PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(softswitch_proto PUBLIC softswitch::options)

# softswitch::runtime: syscalls and processes (shared memory, sockets, packet I/O).
find_package(Threads REQUIRED)
add_library(softswitch_runtime
  src/cp/cli_server.cpp
  src/cp/control_plane.cpp
  src/cp/event_loop.cpp
  src/cp/gui_api.cpp
  src/cp/netlink.cpp
  src/cp/show.cpp
  src/dp/af_packet_port.cpp
  src/dp/dataplane.cpp
  src/gui/http_server.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/gui_assets.cpp
  src/shm/region_mapping.cpp
  src/sys/fd.cpp
  src/sys/realtime.cpp
  src/sys/shared_memory.cpp
  src/sys/unix_socket.cpp
)
add_library(softswitch::runtime ALIAS softswitch_runtime)
target_link_libraries(softswitch_runtime PUBLIC softswitch::proto Threads::Threads)

# The web GUI's static files are compiled into softswitchd.
set(SOFTSWITCH_GUI_ASSETS index.html app.js style.css)
list(TRANSFORM SOFTSWITCH_GUI_ASSETS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/www/" OUTPUT_VARIABLE _gui_asset_paths)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/gui_assets.cpp"
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/gui_assets.cpp
          -DASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src/gui/www "-DASSETS=${SOFTSWITCH_GUI_ASSETS}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake
  DEPENDS ${_gui_asset_paths} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake
  COMMENT "Embedding web GUI assets"
  VERBATIM)

# ---- executables ------------------------------------------------------------
add_executable(softswitchd src/cp/main.cpp)
target_link_libraries(softswitchd PRIVATE softswitch::runtime)
add_executable(ss-dataplane src/dp/main.cpp)
target_link_libraries(ss-dataplane PRIVATE softswitch::runtime)
add_executable(sscli tools/sscli.cpp)
target_link_libraries(sscli PRIVATE softswitch::runtime)

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
install(TARGETS softswitchd ss-dataplane sscli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
