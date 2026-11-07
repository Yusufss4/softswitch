# Interface target that carries project-wide warnings, sanitizer and coverage flags.
# -Wnull-dereference is deliberately absent: GCC 12/13 report false positives on inlined
# std::vector::data() at -O2. Null dereferences are caught by UBSan and clang-tidy instead.
# Link it into every first-party target; third-party code (gtest, benchmark) stays unaffected.
add_library(softswitch_options INTERFACE)
add_library(softswitch::options ALIAS softswitch_options)

target_compile_options(softswitch_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
  -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
if(SOFTSWITCH_WERROR)
  target_compile_options(softswitch_options INTERFACE -Werror)
endif()

if(SOFTSWITCH_SANITIZE)
  list(JOIN SOFTSWITCH_SANITIZE "," _san)
  message(STATUS "softswitch: sanitizers = ${_san}")
  target_compile_options(softswitch_options INTERFACE
    -fsanitize=${_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(softswitch_options INTERFACE -fsanitize=${_san})
endif()

if(SOFTSWITCH_COVERAGE)
  message(STATUS "softswitch: coverage instrumentation enabled")
  target_compile_options(softswitch_options INTERFACE --coverage -O0 -g)
  target_link_options(softswitch_options INTERFACE --coverage)
endif()
