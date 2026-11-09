#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/json.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// Levels of objects and arrays a document may nest: a forged file MUST NOT
// recurse without bound.
constexpr std::size_t max_depth = 32;

// Tells whether the number token text read as zero only because its value is
// too small for a double. strtod gives 0 for "1e-999" without an error.
bool LostToUnderflow(double value, std::string_view text) {
  return value == 0 &&
         text.substr(0, text.find_first_of("eE")).find_first_of("123456789") !=
             std::string_view::npos;
}

// Receives the events of nlohmann's SAX parser and builds a JsonValue. A SAX
// handler sees every key before it is stored, so a repeated key and a nesting
// too deep are refused here, where the DOM parser of nlohmann would keep the
// last key and recurse.
class Builder final : public nlohmann::json_sax<nlohmann::json> {
 public:
  explicit Builder(JsonValue& root) : root_(root) {}

  // Why the parse stopped. Empty when it did not.
  [[nodiscard]] const std::string& Error() const { return error_; }

  bool null() override {
    Place().kind = JsonValue::Kind::Null;
    return true;
  }

  bool boolean(bool value) override {
    JsonValue& slot = Place();
    slot.kind = JsonValue::Kind::Bool;
    slot.boolean = value;
    return true;
  }

  bool number_integer(number_integer_t value) override {
    return Number(static_cast<double>(value));
  }

  bool number_unsigned(number_unsigned_t value) override {
    return Number(static_cast<double>(value));
  }

  bool number_float(number_float_t value, const string_t& text) override {
    // nlohmann refuses an overflow itself, as a parse error.
    if (LostToUnderflow(value, text)) {
      return Fail("a number is too small: " + text);
    }
    return Number(value);
  }

  bool string(string_t& value) override {
    JsonValue& slot = Place();
    slot.kind = JsonValue::Kind::String;
    slot.text = std::move(value);
    return true;
  }

  // A JSON text has no binary value: only the binary formats of nlohmann do.
  bool binary(binary_t& /*value*/) override { return Fail("binary value"); }

  bool start_object(std::size_t /*elements*/) override {
    return Open(JsonValue::Kind::Object);
  }

  bool key(string_t& name) override {
    if (open_.back()->Find(name) != nullptr) {
      return Fail("the key \"" + name + "\" appears twice");
    }
    key_ = std::move(name);
    return true;
  }

  bool end_object() override { return Close(); }

  bool start_array(std::size_t /*elements*/) override {
    return Open(JsonValue::Kind::Array);
  }

  bool end_array() override { return Close(); }

  bool parse_error(std::size_t /*position*/, const std::string& /*token*/,
                   const nlohmann::json::exception& error) override {
    error_ = error.what();
    return false;
  }

 private:
  // Notes why the parse must stop. Gives false, the answer of a handler that
  // refuses.
  bool Fail(std::string message) {
    error_ = std::move(message);
    return false;
  }

  // The place of the next value: the document itself, an element of the open
  // array or the member of the open object that key() named. The reference
  // holds until the next value is placed in the same container.
  JsonValue& Place() {
    if (open_.empty()) {
      return root_;
    }
    JsonValue& parent = *open_.back();
    if (parent.kind == JsonValue::Kind::Array) {
      return parent.items.emplace_back();
    }
    return parent.members.emplace_back(JsonMember{std::move(key_), {}}).value;
  }

  bool Number(double value) {
    JsonValue& slot = Place();
    slot.kind = JsonValue::Kind::Number;
    slot.number = value;
    return true;
  }

  // Starts an object or an array.
  bool Open(JsonValue::Kind kind) {
    if (open_.size() >= max_depth) {
      return Fail("the document nests too deep");
    }
    JsonValue& slot = Place();
    slot.kind = kind;
    open_.push_back(&slot);
    return true;
  }

  bool Close() {
    open_.pop_back();
    return true;
  }

  JsonValue& root_;
  // The objects and arrays being read, the innermost last. A container only
  // grows when the one inside it is closed, so these pointers stay valid.
  std::vector<JsonValue*> open_;
  // The key that the next value of an object goes under.
  std::string key_;
  std::string error_;
};

}  // namespace

const JsonValue* JsonValue::Find(std::string_view key) const {
  for (const auto& [name, member] : members) {
    if (name == key) {
      return &member;
    }
  }
  return nullptr;
}

std::optional<JsonValue> ParseJson(std::string_view text, std::string& error) {
  JsonValue root;
  Builder builder(root);
  // Strict: any byte after the document is an error. The SAX parser reads
  // without recursion and does not throw: a refusal is a false result.
  if (!nlohmann::json::sax_parse(text.data(), text.data() + text.size(),
                                 &builder)) {
    error = builder.Error().empty() ? "the document is not valid JSON"
                                    : builder.Error();
    return std::nullopt;
  }
  return root;
}

// The document being built, and where its open objects and arrays are.
struct JsonWriter::Impl {
  using Json = nlohmann::ordered_json;

  // Puts value where the next value goes and gives its place.
  Json& Place(Json value) {
    if (open.empty()) {
      root = std::move(value);
      return root;
    }
    Json& parent = *open.back();
    if (parent.is_array()) {
      parent.push_back(std::move(value));
      return parent.back();
    }
    // An append, not parent[key]: a repeated key stays in the text, where
    // ParseJson refuses it, and is not hidden by the last value.
    return parent.get_ref<Json::object_t&>()
        .emplace_back(key, std::move(value))
        .second;
  }

  // Keeps the order of the keys as the caller wrote them.
  Json root;
  // The open objects and arrays, the innermost last. A container only grows
  // when the one inside it is closed, so these pointers stay valid.
  std::vector<Json*> open;
  // The key that the next value of the open object goes under.
  std::string key;
};

JsonWriter::JsonWriter() : impl_(std::make_unique<Impl>()) {}

JsonWriter::JsonWriter(JsonWriter&&) noexcept = default;

JsonWriter& JsonWriter::operator=(JsonWriter&&) noexcept = default;

JsonWriter::~JsonWriter() = default;

void JsonWriter::BeginObject() {
  impl_->open.push_back(&impl_->Place(Impl::Json::object()));
}

void JsonWriter::EndObject() { impl_->open.pop_back(); }

void JsonWriter::BeginArray() {
  impl_->open.push_back(&impl_->Place(Impl::Json::array()));
}

void JsonWriter::EndArray() { impl_->open.pop_back(); }

void JsonWriter::Key(std::string_view key) { impl_->key = key; }

void JsonWriter::String(std::string_view value) {
  impl_->Place(std::string(value));
}

void JsonWriter::Number(std::uint64_t value) { impl_->Place(value); }

void JsonWriter::Number(std::int64_t value) { impl_->Place(value); }

void JsonWriter::Number(double value) { impl_->Place(value); }

void JsonWriter::Bool(bool value) { impl_->Place(value); }

void JsonWriter::Null() { impl_->Place(nullptr); }

std::string JsonWriter::Take() {
  // Bytes that are not UTF-8 (a file name read as WTF-8) become U+FFFD: the
  // document stays valid JSON for the python script.
  std::string text = impl_->root.dump(
      2, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
  text.push_back('\n');
  return text;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
