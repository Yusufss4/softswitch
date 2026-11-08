#include <gtest/gtest.h>

#include "softswitch/gui/json.hpp"

namespace softswitch::gui {
namespace {

TEST(JsonWriter, NestedStructures) {
  JsonWriter w;
  w.begin_object()
      .field("name", "sw1")
      .field("ports", 4)
      .field("up", true)
      .key("list")
      .begin_array()
      .value(1)
      .value("two")
      .begin_object()
      .field("x", std::uint64_t{3})
      .end_object()
      .end_array()
      .key("none")
      .null()
      .end_object();
  EXPECT_EQ(w.str(), R"({"name":"sw1","ports":4,"up":true,"list":[1,"two",{"x":3}],"none":null})");
}

TEST(JsonWriter, EscapesStrings) {
  JsonWriter w;
  w.begin_array().value("a\"b\\c\nd\x01").end_array();
  EXPECT_EQ(w.str(), R"(["a\"b\\c\nd\u0001"])");
}

TEST(JsonWriter, EmptyContainers) {
  JsonWriter w;
  w.begin_object()
      .key("a")
      .begin_array()
      .end_array()
      .key("o")
      .begin_object()
      .end_object()
      .end_object();
  EXPECT_EQ(w.str(), R"({"a":[],"o":{}})");
}

}  // namespace
}  // namespace softswitch::gui
