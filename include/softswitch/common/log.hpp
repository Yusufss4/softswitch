#pragma once

/// @file
/// Minimal leveled logging to stderr. Not for the data-plane hot path.

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace softswitch::log {

enum class Level : std::uint8_t { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void set_level(Level level) noexcept;
void set_tag(std::string_view tag);  ///< Prefix such as "sw1/cp".
[[nodiscard]] bool enabled(Level level) noexcept;
void write(Level level, const std::string& message);

template <typename... Args>
void emit(Level level, const Args&... args) {
  if (!enabled(level)) {
    return;
  }
  std::ostringstream out;
  // String literals are streamed as const char*, which is the point.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  (out << ... << args);
  write(level, out.str());
}

template <typename... Args>
void debug(const Args&... args) {
  emit(Level::kDebug, args...);
}
template <typename... Args>
void info(const Args&... args) {
  emit(Level::kInfo, args...);
}
template <typename... Args>
void warn(const Args&... args) {
  emit(Level::kWarn, args...);
}
template <typename... Args>
void error(const Args&... args) {
  emit(Level::kError, args...);
}

}  // namespace softswitch::log
