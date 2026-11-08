// Pure HTTP helpers (no sockets), unit-tested and fuzzed.
#include <string>

#include "softswitch/gui/http.hpp"

namespace softswitch::gui {
namespace {

const char* reason(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Request Header Fields Too Large";
    default:
      return "Error";
  }
}

}  // namespace

std::optional<HttpRequest> parse_request_head(std::string_view head) {
  const auto line_end = head.find("\r\n");
  if (line_end == std::string_view::npos || head.size() > kMaxRequestHead) {
    return std::nullopt;
  }
  const std::string_view line = head.substr(0, line_end);
  const auto sp1 = line.find(' ');
  const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0 ||
      sp2 == sp1 + 1) {
    return std::nullopt;
  }
  const std::string_view version = line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    return std::nullopt;
  }
  const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (target.empty() || target.front() != '/') {
    return std::nullopt;
  }
  for (const char c : target) {
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
      return std::nullopt;
    }
  }
  HttpRequest req;
  req.method = std::string(line.substr(0, sp1));
  const auto q = target.find('?');
  req.path = std::string(target.substr(0, q));
  if (q != std::string_view::npos) {
    req.query = std::string(target.substr(q + 1));
  }
  return req;
}

std::string render_response(const HttpResponse& r) {
  std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
  out += "Content-Type: " + r.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
  out += "Access-Control-Allow-Origin: *\r\n";
  out += "Cache-Control: no-store\r\n";
  out += "X-Content-Type-Options: nosniff\r\n";
  out += "Connection: close\r\n\r\n";
  out += r.body;
  return out;
}

}  // namespace softswitch::gui
