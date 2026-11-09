#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/json.h>

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

using Catch::Matchers::ContainsSubstring;
using ToolDeviceAccess::JsonValue;
using ToolDeviceAccess::JsonWriter;
using ToolDeviceAccess::ParseJson;
using Kind = ToolDeviceAccess::JsonValue::Kind;

namespace {

// Parses text, which the test cannot go on without.
JsonValue Parse(std::string_view text) {
  std::string error;
  std::optional<JsonValue> value = ParseJson(text, error);
  INFO("text: " << text << "\nerror: " << error);
  REQUIRE(value.has_value());
  return std::move(*value);
}

// Why text does not parse, or an empty string when it does.
std::string Refusal(std::string_view text) {
  std::string error;
  const std::optional<JsonValue> value = ParseJson(text, error);
  if (value.has_value()) {
    return {};
  }
  REQUIRE_FALSE(error.empty());
  return error;
}

// n arrays or objects inside one another.
std::string Nested(size_t depth, bool objects) {
  std::string text;
  for (size_t level = 0; level < depth; level++) {
    text += objects ? "{\"k\":" : "[";
  }
  text += "1";
  for (size_t level = 0; level < depth; level++) {
    text += objects ? "}" : "]";
  }
  return text;
}

// The text value of a one-string document.
std::string StringOf(std::string_view body) {
  const JsonValue value = Parse('"' + std::string(body) + '"');
  REQUIRE(value.kind == Kind::String);
  return value.text;
}

}  // namespace

TEST_CASE("ParseJson reads every kind of value", "[device-access][json]") {
  CHECK(Parse("null").kind == Kind::Null);
  CHECK(Parse("true").boolean);
  CHECK(Parse("true").kind == Kind::Bool);
  CHECK_FALSE(Parse("false").boolean);
  CHECK(Parse("false").kind == Kind::Bool);
  CHECK(Parse("  \t\r\n 42 \n").number == 42);
  CHECK(Parse("\"text\"").text == "text");
  CHECK(Parse("[]").kind == Kind::Array);
  CHECK(Parse("{}").kind == Kind::Object);

  const JsonValue value = Parse(
      "{\"a\": [1, 2, {\"b\": null}], \"c\": {\"d\": true}, \"e\": \"x\"}");
  REQUIRE(value.kind == Kind::Object);
  REQUIRE(value.members.size() == 3);
  // The members keep the order of the document.
  CHECK(value.members.at(0).key == "a");
  CHECK(value.members.at(1).key == "c");
  CHECK(value.members.at(2).key == "e");
  const JsonValue* const array = value.Find("a");
  REQUIRE(array != nullptr);
  REQUIRE(array->items.size() == 3);
  CHECK(array->items.at(1).number == 2);
  CHECK(array->items.at(2).Find("b")->kind == Kind::Null);
  CHECK(value.Find("c")->Find("d")->boolean);
  CHECK(value.Find("missing") == nullptr);
  CHECK(Parse("1").Find("a") == nullptr);
}

TEST_CASE("ParseJson reads numbers", "[device-access][json]") {
  CHECK(Parse("0").number == 0);
  CHECK(Parse("-0").number == 0);
  CHECK(Parse("-17").number == -17);
  CHECK(Parse("3.25").number == 3.25);
  CHECK(Parse("1e3").number == 1000);
  CHECK(Parse("1E+3").number == 1000);
  CHECK(Parse("25e-1").number == 2.5);
  CHECK(Parse("9007199254740992").number == 9007199254740992.0);
  CHECK(Parse("18446744073709551615").number == 18446744073709551615.0);
}

TEST_CASE("ParseJson refuses a number out of range or malformed",
          "[device-access][json]") {
  for (const std::string_view text :
       {"1e999", "-1e999", "1e-999", "1e400", "-", "+1", "1.2.3", "1e", "--1",
        ".", "e5", "1-2", "0x10", "NaN", "Infinity", "-Infinity", "1,5"}) {
    INFO("text " << text);
    CHECK_FALSE(Refusal(text).empty());
  }
  CHECK_THAT(Refusal("1e999"), ContainsSubstring("number overflow"));
  CHECK_THAT(Refusal("-1e999"), ContainsSubstring("number overflow"));
  CHECK_THAT(Refusal("1e-999"), ContainsSubstring("too small"));
}

TEST_CASE("ParseJson refuses a document with trailing data",
          "[device-access][json]") {
  for (const std::string_view text :
       {"{} x", "1 2", "{}}", "[] []", "null null", "true false", "\"a\"\"b\"",
        "{},", "[1]]", "0 0"}) {
    INFO("text " << text);
    CHECK_THAT(Refusal(text), ContainsSubstring("expected end of input"));
  }
  // The position of the garbage.
  CHECK_THAT(Refusal("{} x"), ContainsSubstring("column 4"));
  CHECK_THAT(Refusal("  1 2"), ContainsSubstring("column 5"));
  // White space after the document is not garbage.
  CHECK(Parse("{}  \n\t\r ").kind == Kind::Object);
}

TEST_CASE("ParseJson refuses a cut or malformed document",
          "[device-access][json]") {
  for (const std::string_view text : {"",
                                      "   ",
                                      "{",
                                      "[",
                                      "[1",
                                      "[1,",
                                      "{\"a\"",
                                      "{\"a\":",
                                      "{\"a\":1",
                                      "{\"a\":1,",
                                      "{\"a\" 1}",
                                      "{a:1}",
                                      "{\"a\":1 \"b\":2}",
                                      "[1 2]",
                                      "[1,]x",
                                      "{,}",
                                      "[,]",
                                      "tru",
                                      "nul",
                                      "fals",
                                      "True",
                                      "'a'",
                                      "\"unclosed",
                                      "{\"a\":1,}",
                                      "{1:2}",
                                      "}",
                                      "]",
                                      ":"}) {
    INFO("text \"" << text << "\"");
    CHECK_FALSE(Refusal(text).empty());
  }
  CHECK_THAT(Refusal(""), ContainsSubstring("empty input"));
  CHECK_THAT(Refusal("{\"a\" 1}"), ContainsSubstring("expected ':'"));
  CHECK_THAT(Refusal("[1 2]"), ContainsSubstring("expected ']'"));
  CHECK_THAT(Refusal("{\"a\":1 \"b\":2}"), ContainsSubstring("expected '}'"));
  CHECK_THAT(Refusal("\"unclosed"), ContainsSubstring("missing closing quote"));
}

TEST_CASE("ParseJson takes a document nested 32 levels and refuses 33",
          "[device-access][json]") {
  for (const bool objects : {false, true}) {
    INFO("objects " << objects);
    CHECK(Parse(Nested(32, objects)).kind ==
          (objects ? Kind::Object : Kind::Array));
    const std::string refusal = Refusal(Nested(33, objects));
    CHECK_THAT(refusal, ContainsSubstring("nests too deep"));
    // A forged file MUST NOT recurse without bound.
    CHECK_THAT(Refusal(Nested(100000, objects)),
               ContainsSubstring("nests too deep"));
  }
  std::string mixed;
  for (size_t level = 0; level < 20; level++) {
    mixed += "[{\"k\":";
  }
  mixed += "1";
  for (size_t level = 0; level < 20; level++) {
    mixed += "}]";
  }
  CHECK_THAT(Refusal(mixed), ContainsSubstring("nests too deep"));
}

TEST_CASE("ParseJson refuses a key that appears twice in one object",
          "[device-access][json]") {
  const std::string refusal = Refusal("{\"a\":1,\"b\":2,\"a\":3}");
  CHECK_THAT(refusal, ContainsSubstring("the key \"a\" appears twice"));

  // The same key in two objects, or in an object and its child, is not a
  // duplicate.
  const JsonValue value = Parse("{\"a\":{\"a\":1},\"b\":{\"a\":2}}");
  CHECK(value.Find("a")->Find("a")->number == 1);
  CHECK(value.Find("b")->Find("a")->number == 2);
  // Two keys that differ by their escapes only are the same key.
  CHECK_THAT(Refusal("{\"a\":1,\"\\u0061\":2}"),
             ContainsSubstring("appears twice"));
  // Keys that differ by case are two.
  CHECK(Parse("{\"a\":1,\"A\":2}").members.size() == 2);
}

TEST_CASE("ParseJson reads the escapes of a string", "[device-access][json]") {
  CHECK(StringOf(R"(a\"b)") == "a\"b");
  CHECK(StringOf(R"(a\\b)") == "a\\b");
  CHECK(StringOf(R"(a\/b)") == "a/b");
  CHECK(StringOf(R"(\b\f\n\r\t)") == "\b\f\n\r\t");
  CHECK(StringOf(R"(\u0041\u0062)") == "Ab");
  CHECK(StringOf(R"(\u0000)") == std::string(1, '\0'));
  // Two, three and four bytes of UTF-8.
  CHECK(StringOf(R"(\u00e9)") == "\xC3\xA9");
  CHECK(StringOf(R"(\u20AC)") == "\xE2\x82\xAC");
  CHECK(StringOf(R"(\ud83d\ude00)") == "\xF0\x9F\x98\x80");
  // Bytes of UTF-8 pass as they are.
  CHECK(StringOf("caf\xC3\xA9") == "caf\xC3\xA9");
}

TEST_CASE("ParseJson refuses a malformed escape", "[device-access][json]") {
  CHECK_THAT(Refusal(R"("\x")"),
             ContainsSubstring("forbidden character after backslash"));
  CHECK_THAT(Refusal(R"("\U0041")"),
             ContainsSubstring("forbidden character after backslash"));
  CHECK_THAT(Refusal(R"("\u12")"), ContainsSubstring("4 hex digits"));
  CHECK_THAT(Refusal(R"("\u12G4")"), ContainsSubstring("4 hex digits"));
  CHECK_THAT(Refusal(R"("\u")"), ContainsSubstring("4 hex digits"));
  CHECK_FALSE(Refusal("\"\\").empty());
  CHECK_FALSE(Refusal("\"abc\\").empty());
}

TEST_CASE("ParseJson refuses a lone or misplaced surrogate",
          "[device-access][json]") {
  // A high surrogate needs a low one right after it.
  CHECK_THAT(Refusal(R"("\ud83d")"),
             ContainsSubstring("must be followed by U+DC00..U+DFFF"));
  CHECK_THAT(Refusal(R"("\ud83dx")"),
             ContainsSubstring("must be followed by U+DC00..U+DFFF"));
  CHECK_THAT(Refusal(R"("\ud83d\u0041")"),
             ContainsSubstring("must be followed by U+DC00..U+DFFF"));
  CHECK_THAT(Refusal(R"("\ud83d\ud83d")"),
             ContainsSubstring("must be followed by U+DC00..U+DFFF"));
  CHECK_THAT(Refusal(R"("\ud83d\n")"),
             ContainsSubstring("must be followed by U+DC00..U+DFFF"));
  // A low surrogate alone, or before its high one.
  CHECK_THAT(Refusal(R"("\ude00")"),
             ContainsSubstring("must follow U+D800..U+DBFF"));
  CHECK_THAT(Refusal(R"("\ude00\ud83d")"),
             ContainsSubstring("must follow U+D800..U+DBFF"));
  // The bounds of the ranges.
  CHECK(StringOf(R"(\ud7ff)") == "\xED\x9F\xBF");
  CHECK(StringOf(R"(\ue000)") == "\xEE\x80\x80");
  CHECK(StringOf(R"(\ud800\udc00)") == "\xF0\x90\x80\x80");
  CHECK(StringOf(R"(\udbff\udfff)") == "\xF4\x8F\xBF\xBF");
  CHECK_FALSE(Refusal(R"("\ud800\udbff")").empty());
  CHECK_FALSE(Refusal(R"("\udc00")").empty());
  CHECK_FALSE(Refusal(R"("\udfff")").empty());
  CHECK_FALSE(Refusal(R"("\udbff")").empty());
}

TEST_CASE("ParseJson refuses a control character in a string",
          "[device-access][json]") {
  for (const char control : {'\0', '\x01', '\n', '\r', '\t', '\x1f'}) {
    INFO("control " << static_cast<int>(control));
    CHECK_THAT(Refusal(std::string("\"a") + control + "b\""),
               ContainsSubstring("control character"));
  }
  // The bytes from 0x20 on, and valid UTF-8, are text.
  CHECK(StringOf(" ~\x7f\xEF\xBF\xBF") == " ~\x7f\xEF\xBF\xBF");
}

TEST_CASE("ParseJson refuses a string that is not UTF-8",
          "[device-access][json]") {
  for (const std::string_view body :
       {"\xFF", "\xC3", "\xC3(", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80",
        "\x80"}) {
    INFO("body " << body);
    CHECK_THAT(Refusal('"' + std::string(body) + '"'),
               ContainsSubstring("UTF-8"));
  }
}

TEST_CASE("JsonWriter writes an indented document", "[device-access][json]") {
  JsonWriter json;
  json.BeginObject();
  json.Field("name", "value");
  json.Field("count", std::uint64_t{7});
  json.Field("flag", true);
  json.Key("list");
  json.BeginArray();
  json.Number(std::uint64_t{1});
  json.Number(std::int64_t{-2});
  json.Number(0.5);
  json.Null();
  json.Bool(false);
  json.String("x");
  json.EndArray();
  json.Key("empty_object");
  json.BeginObject();
  json.EndObject();
  json.Key("empty_array");
  json.BeginArray();
  json.EndArray();
  json.Key("nested");
  json.BeginObject();
  json.Field("deep", "yes");
  json.EndObject();
  json.EndObject();

  CHECK(json.Take() ==
        "{\n"
        "  \"name\": \"value\",\n"
        "  \"count\": 7,\n"
        "  \"flag\": true,\n"
        "  \"list\": [\n"
        "    1,\n"
        "    -2,\n"
        "    0.5,\n"
        "    null,\n"
        "    false,\n"
        "    \"x\"\n"
        "  ],\n"
        "  \"empty_object\": {},\n"
        "  \"empty_array\": [],\n"
        "  \"nested\": {\n"
        "    \"deep\": \"yes\"\n"
        "  }\n"
        "}\n");
}

TEST_CASE("JsonWriter escapes what a string MUST escape",
          "[device-access][json]") {
  JsonWriter json;
  json.BeginObject();
  json.Field("quote\"key", "a\"b\\c\nd\re\tf");
  json.Field("control", std::string("\x01\x1f\x7f", 3));
  json.Field("utf8", "caf\xC3\xA9 \xF0\x9F\x98\x80");
  json.EndObject();
  const std::string text = json.Take();

  CHECK_THAT(text, ContainsSubstring(R"("quote\"key": "a\"b\\c\nd\re\tf")"));
  CHECK_THAT(text, ContainsSubstring(R"("control": "\u0001\u001f)"));
  CHECK_THAT(text, ContainsSubstring("caf\xC3\xA9 \xF0\x9F\x98\x80"));
}

TEST_CASE("A document the writer wrote parses back", "[device-access][json]") {
  const std::string text_with_everything =
      std::string("line\nbreak \"quoted\" back\\slash tab\t \x01 nul") +
      std::string(1, '\0') + "end caf\xC3\xA9 \xF0\x9F\x98\x80";

  JsonWriter json;
  json.BeginObject();
  json.Field("text", text_with_everything);
  json.Field("big", std::uint64_t{9'007'199'254'740'992ULL});
  json.Key("negative");
  json.Number(std::int64_t{-9'007'199'254'740'992LL});
  json.Key("fraction");
  json.Number(0.125);
  json.Key("tiny");
  json.Number(1.5e-7);
  json.Key("huge");
  json.Number(2.5e21);
  json.Field("yes", true);
  json.Key("no");
  json.Bool(false);
  json.Key("nothing");
  json.Null();
  json.Key("items");
  json.BeginArray();
  for (std::uint64_t i = 0; i < 5; i++) {
    json.BeginObject();
    json.Field("i", i);
    json.Key("sub");
    json.BeginArray();
    json.String("a");
    json.BeginArray();
    json.EndArray();
    json.EndArray();
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();

  const JsonValue root = Parse(json.Take());
  CHECK(root.Find("text")->text == text_with_everything);
  CHECK(root.Find("big")->number == 9007199254740992.0);
  CHECK(root.Find("negative")->number == -9007199254740992.0);
  CHECK(root.Find("fraction")->number == 0.125);
  CHECK(root.Find("tiny")->number == 1.5e-7);
  CHECK(root.Find("huge")->number == 2.5e21);
  CHECK(root.Find("yes")->boolean);
  CHECK_FALSE(root.Find("no")->boolean);
  CHECK(root.Find("nothing")->kind == Kind::Null);
  const JsonValue& items = *root.Find("items");
  REQUIRE(items.items.size() == 5);
  CHECK(items.items.at(3).Find("i")->number == 3);
  CHECK(items.items.at(4).Find("sub")->items.at(1).kind == Kind::Array);
}

TEST_CASE("A document nested to the limit parses back after the writer",
          "[device-access][json]") {
  JsonWriter json;
  constexpr size_t levels = 32;
  for (size_t level = 0; level < levels; level++) {
    json.BeginArray();
  }
  json.Number(std::uint64_t{1});
  for (size_t level = 0; level < levels; level++) {
    json.EndArray();
  }

  const JsonValue root = Parse(json.Take());
  const JsonValue* at = &root;
  for (size_t level = 0; level < levels; level++) {
    REQUIRE(at->kind == Kind::Array);
    REQUIRE(at->items.size() == 1);
    at = &at->items.front();
  }
  CHECK(at->number == 1);
}

TEST_CASE("JsonWriter keeps a document valid", "[device-access][json]") {
  JsonWriter json;
  json.BeginArray();
  json.Number(std::numeric_limits<double>::infinity());
  json.Number(std::numeric_limits<double>::quiet_NaN());
  json.String(
      "a\xFF"
      "b");
  json.String(
      "ed\xA0\x80"
      "x");
  json.EndArray();

  const JsonValue root = Parse(json.Take());
  REQUIRE(root.items.size() == 4);
  CHECK(root.items.at(0).kind == Kind::Null);
  CHECK(root.items.at(1).kind == Kind::Null);
  CHECK(root.items.at(2).text ==
        "a\xEF\xBF\xBD"
        "b");
  CHECK(root.items.at(3).text ==
        "ed\xEF\xBF\xBD\xEF\xBF\xBD"
        "x");
}

TEST_CASE("The writer of a duplicate key makes a document the parser refuses",
          "[device-access][json]") {
  JsonWriter json;
  json.BeginObject();
  json.Field("same", std::uint64_t{1});
  json.Field("same", std::uint64_t{2});
  json.EndObject();

  CHECK_THAT(Refusal(json.Take()), ContainsSubstring("appears twice"));
}
