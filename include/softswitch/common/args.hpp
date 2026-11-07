#pragma once

/// @file
/// Minimal command-line cursor shared by the executables.

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace softswitch {

/// Walks argv (skipping argv[0]); a flag's value is consumed with value().
class ArgReader {
 public:
  explicit ArgReader(std::span<char*> args) noexcept : args_(args) {}

  /// The next argument, or nullopt at the end.
  [[nodiscard]] std::optional<std::string_view> next() noexcept {
    if (pos_ >= args_.size()) {
      return std::nullopt;
    }
    return std::string_view{args_[pos_++]};
  }

  /// The argument after @p flag. Throws std::invalid_argument if there is none.
  [[nodiscard]] std::string value(std::string_view flag) {
    if (pos_ >= args_.size()) {
      throw std::invalid_argument(std::string(flag) + " needs a value");
    }
    return args_[pos_++];
  }

 private:
  std::span<char*> args_;
  std::size_t pos_ = 1;
};

}  // namespace softswitch
