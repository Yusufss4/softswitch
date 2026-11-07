#include "softswitch/common/log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace softswitch::log {
namespace {

std::atomic<Level>& current_level() {
  static std::atomic<Level> level{Level::kInfo};
  return level;
}

struct TagState {
  std::mutex mutex;
  std::string tag;
};

TagState& tag_state() {
  static TagState state;
  return state;
}

constexpr std::array<std::string_view, 4> kLevelNames{"debug", "info", "warn", "error"};

}  // namespace

void set_level(Level level) noexcept { current_level().store(level); }

void set_tag(std::string_view tag) {
  auto& state = tag_state();
  const std::scoped_lock lock(state.mutex);
  state.tag = tag;
}

bool enabled(Level level) noexcept { return level >= current_level().load(); }

void write(Level level, const std::string& message) {
  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  ::localtime_r(&secs, &tm);
  std::array<char, 16> clock{};
  (void)std::strftime(clock.data(), clock.size(), "%H:%M:%S", &tm);

  auto& state = tag_state();
  const std::scoped_lock lock(state.mutex);
  std::string line;
  line.reserve(message.size() + 48);
  line.append(clock.data()).append(".");
  const auto ms = std::to_string(millis);
  line.append(3 - ms.size(), '0').append(ms).append(" ");
  line.append(kLevelNames[static_cast<std::size_t>(level)]).append(" ");
  if (!state.tag.empty()) {
    line.append("[").append(state.tag).append("] ");
  }
  line.append(message).append("\n");
  (void)std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace softswitch::log
