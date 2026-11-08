#include "softswitch/gui/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

namespace softswitch::gui {

void JsonWriter::separator() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!first_.empty()) {
    if (!first_.back()) {
      out_.push_back(',');
    }
    first_.back() = false;
  }
}

JsonWriter& JsonWriter::begin_object() {
  separator();
  out_.push_back('{');
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  out_.push_back('}');
  first_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  separator();
  out_.push_back('[');
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  out_.push_back(']');
  first_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
  separator();
  append_string(out_, name);
  out_.push_back(':');
  after_key_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
  separator();
  append_string(out_, text);
  return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number) {
  separator();
  out_ += std::to_string(number);
  return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
  separator();
  out_ += std::to_string(number);
  return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
  separator();
  out_ += flag ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::value(double number) {
  if (!std::isfinite(number)) {
    return null();
  }
  separator();
  std::array<char, 32> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), number);
  out_.append(buf.data(), ec == std::errc{} ? end : buf.data());
  return *this;
}

JsonWriter& JsonWriter::null() {
  separator();
  out_ += "null";
  return *this;
}

void JsonWriter::append_string(std::string& out, std::string_view text) {
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  out.push_back('"');
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[c >> 4U]);
          out.push_back(kHex[c & 0xFU]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

}  // namespace softswitch::gui
