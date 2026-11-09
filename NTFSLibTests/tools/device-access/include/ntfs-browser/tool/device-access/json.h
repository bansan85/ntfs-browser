#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

struct JsonMember;

// A parsed JSON value. The profile and the benchmark summary are small and
// flat, so this holds a whole document in memory. It is plain data:
// nlohmann/json reads and writes the text in json.cpp and no header shows it.
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
// byte, a nesting deeper than 32 levels, a byte that is not UTF-8 in a string
// and any malformed token are errors: the result is empty and error says why.
// A syntax error says where, as a line and a column.
[[nodiscard]] std::optional<JsonValue> ParseJson(std::string_view text,
                                                 std::string& error);

// Builds a JSON document, indented by two spaces. Commas and colons are the
// writer's: the caller opens and closes objects and arrays, names members with
// Key(), and gives values. The members keep the order they were written in.
class JsonWriter {
 public:
  // Starts an empty document. The state sits behind a pointer, so that no
  // header shows nlohmann/json.
  JsonWriter();
  JsonWriter(const JsonWriter&) = delete;
  JsonWriter& operator=(const JsonWriter&) = delete;
  JsonWriter(JsonWriter&&) noexcept;
  JsonWriter& operator=(JsonWriter&&) noexcept;
  ~JsonWriter();

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

  // The document so far, with a final newline. A number that is not finite
  // is written as null, and a byte that is not UTF-8 as U+FFFD.
  [[nodiscard]] std::string Take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
