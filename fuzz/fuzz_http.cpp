// The HTTP request-head parser faces the network (the GUI port): arbitrary bytes must never
// crash it, and anything it accepts must be a GET-able path without control characters.
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "softswitch/gui/http.hpp"
#include "softswitch/gui/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  if (const auto req = softswitch::gui::parse_request_head(text)) {
    if (req->path.empty() || req->path.front() != '/') std::abort();
    for (const char c : req->path) {
      if (static_cast<unsigned char>(c) <= 0x20) std::abort();
    }
  }
  // JSON string escaping must never produce raw control characters.
  std::string out;
  softswitch::gui::JsonWriter::append_string(out, text);
  for (const char c : out) {
    if (static_cast<unsigned char>(c) < 0x20) std::abort();
  }
  return 0;
}
