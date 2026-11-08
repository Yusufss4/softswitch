#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

TEST(JsonWriter, Doubles) {
  JsonWriter w;
  w.begin_array()
      .value(0.5)
      .value(5.65685)
      .value(-2.0)
      .value(1e-7)
      .value(std::nan(""))
      .value(std::numeric_limits<double>::infinity());
  w.end_array();
  EXPECT_EQ(w.str(), "[0.5,5.65685,-2,1e-07,null,null]");
}

}  // namespace
}  // namespace softswitch::gui
