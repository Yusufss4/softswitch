#include <gtest/gtest.h>

#include "softswitch/gui/http.hpp"

namespace softswitch::gui {
namespace {

TEST(Http, ParsesRequestLineAndQuery) {
  const auto r = parse_request_head("GET /api/state?x=1&y=2 HTTP/1.1\r\nHost: a\r\n\r\n");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->method, "GET");
  EXPECT_EQ(r->path, "/api/state");
  EXPECT_EQ(r->query, "x=1&y=2");
}

TEST(Http, RejectsMalformedRequests) {
  for (const char* bad : {"GET\r\n\r\n", "GET / HTTP/2\r\n\r\n", "GET noslash HTTP/1.1\r\n\r\n",
                          "GET  / HTTP/1.1\r\n\r\n", " / HTTP/1.1\r\n\r\n",
                          "GET /a\x01 HTTP/1.1\r\n\r\n", "GET / HTTP/1.1"}) {
    EXPECT_FALSE(parse_request_head(bad).has_value()) << bad;
  }
  EXPECT_FALSE(parse_request_head(std::string(kMaxRequestHead + 1, 'a')).has_value());
}

TEST(Http, RendersCompleteResponses) {
  const auto out = render_response({200, "application/json", "{}"});
  EXPECT_EQ(out.substr(0, 17), "HTTP/1.1 200 OK\r\n");
  EXPECT_NE(out.find("Content-Length: 2\r\n"), std::string::npos);
  EXPECT_NE(out.find("Access-Control-Allow-Origin: *\r\n"), std::string::npos);
  EXPECT_EQ(out.substr(out.size() - 6), "\r\n\r\n{}");
}

}  // namespace
}  // namespace softswitch::gui
