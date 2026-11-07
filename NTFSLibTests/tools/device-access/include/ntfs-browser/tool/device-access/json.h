#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NtfsDevice {

struct JsonMember;

// A parsed JSON value. The profile and the benchmark summary are small and
// flat, so this holds a whole document in memory, with no dependency.
struct JsonValue {
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Object, Array };

  Kind kind{Kind::Null};
  bool boolean{false};
  double number{0};
  std::string text;
  // The members of an object, in document order.
  std::vector<JsonMember> members;
  // The elements of an array.
  std::vector<JsonValue> items;

  // The member called key of an object, or null.
  [[nodiscard]] const JsonValue* Find(std::string_view key) const;
};

// One member of an object. Not a std::pair, which needs a complete JsonValue
// where the vector above is declared.
struct JsonMember {
  std::string key;
  JsonValue value;
};

// Parses text as one JSON document. A duplicate key in an object, a trailing
// byte, a nesting deeper than 32 levels and any malformed token are errors:
// the result is empty and error says where.
[[nodiscard]] std::optional<JsonValue> ParseJson(std::string_view text,
                                                 std::string& error);

// Builds a JSON document, indented by two spaces. Commas and colons are the
// writer's: the caller opens and closes objects and arrays, names members with
// Key(), and gives values.
class JsonWriter {
 public:
  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();
  // Names the next value, inside an object.
  void Key(std::string_view key);
  void String(std::string_view value);
  void Number(std::uint64_t value);
  void Number(std::int64_t value);
  void Number(double value);
  void Bool(bool value);
  void Null();

  // Key() then a value, for the common case.
  void Field(std::string_view key, std::string_view value) {
    Key(key);
    String(value);
  }

  void Field(std::string_view key, const char* value) {
    Key(key);
    String(value);
  }

  void Field(std::string_view key, std::uint64_t value) {
    Key(key);
    Number(value);
  }

  void Field(std::string_view key, bool value) {
    Key(key);
    Bool(value);
  }

  // The document so far, with a final newline.
  [[nodiscard]] std::string Take();

 private:
  // Writes the separator and indentation that precede a value or a key.
  void Separate();

  // One open object or array, and whether it holds an element yet.
  struct Level {
    bool object;
    bool empty;
  };

  std::string out_;
  std::vector<Level> levels_;
  // A key was written, and its value is next.
  bool after_key_{false};
};

}  // namespace NtfsDevice
