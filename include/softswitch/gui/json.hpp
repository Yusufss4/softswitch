#pragma once

/// @file
/// A tiny streaming JSON writer for the GUI API (no parsing, no allocation per value beyond the
/// output string).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace softswitch::gui {

class JsonWriter {
 public:
  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  /// Object key; must be followed by exactly one value.
  JsonWriter& key(std::string_view name);
  JsonWriter& value(std::string_view text);
  JsonWriter& value(const char* text) { return value(std::string_view(text)); }
  JsonWriter& value(std::uint64_t number);
  JsonWriter& value(std::int64_t number);
  JsonWriter& value(int number) { return value(static_cast<std::int64_t>(number)); }
  JsonWriter& value(unsigned number) { return value(static_cast<std::uint64_t>(number)); }
  JsonWriter& value(bool flag);
  JsonWriter& null();

  /// Shorthand for key(name).value(v).
  template <typename T>
  JsonWriter& field(std::string_view name, const T& v) {
    key(name);
    return value(v);
  }

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] std::string take() noexcept { return std::move(out_); }

  /// Escapes @p text as a JSON string literal (with quotes).
  static void append_string(std::string& out, std::string_view text);

 private:
  void separator();

  std::string out_;
  std::vector<bool> first_;  ///< Per nesting level: no element written yet.
  bool after_key_ = false;
};

}  // namespace softswitch::gui
