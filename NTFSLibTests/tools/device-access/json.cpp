#include "json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace NtfsDevice {

namespace {

// Levels of objects and arrays a document may nest: a forged file MUST NOT
// recurse without bound.
constexpr std::size_t max_depth = 32;

// A recursive-descent parser over one document.
class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<JsonValue> Run(std::string& error) {
    SkipSpace();
    JsonValue value;
    if (!ParseValue(value, 0)) {
      error = error_ + " (at byte " + std::to_string(position_) + ")";
      return std::nullopt;
    }
    SkipSpace();
    if (position_ != text_.size()) {
      error = "unexpected data after the document (at byte " +
              std::to_string(position_) + ")";
      return std::nullopt;
    }
    return value;
  }

 private:
  bool Fail(std::string message) {
    error_ = std::move(message);
    return false;
  }

  void SkipSpace() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\t' ||
            text_[position_] == '\n' || text_[position_] == '\r')) {
      position_++;
    }
  }

  bool Consume(char expected) {
    if (position_ < text_.size() && text_[position_] == expected) {
      position_++;
      return true;
    }
    return false;
  }

  bool ParseValue(JsonValue& value, std::size_t depth) {
    if (position_ >= text_.size()) {
      return Fail("the document ends inside a value");
    }
    const char first = text_[position_];
    if (first == '{') {
      return ParseObject(value, depth);
    }
    if (first == '[') {
      return ParseArray(value, depth);
    }
    if (first == '"') {
      value.kind = JsonValue::Kind::String;
      return ParseString(value.text);
    }
    if (ParseWord("true")) {
      value.kind = JsonValue::Kind::Bool;
      value.boolean = true;
      return true;
    }
    if (ParseWord("false")) {
      value.kind = JsonValue::Kind::Bool;
      value.boolean = false;
      return true;
    }
    if (ParseWord("null")) {
      value.kind = JsonValue::Kind::Null;
      return true;
    }
    return ParseNumber(value);
  }

  bool ParseWord(std::string_view word) {
    if (text_.substr(position_, word.size()) == word) {
      position_ += word.size();
      return true;
    }
    return false;
  }

  // A short decimal, -?d+[.d+][(e|E)[+-]d+], whose mantissa has at most 15
  // digits and whose power of ten stays within 10^22. Mantissa and power are
  // exact doubles, so one multiplication or division gives the value from_chars
  // gives: both round correctly. Anything else is left to from_chars.
  static bool ParseShortDecimal(const char* first, const char* last,
                                double& out) {
    bool negative = false;
    if (first != last && *first == '-') {
      negative = true;
      ++first;
    }
    if (first == last || *first == '.') {
      return false;
    }
    std::int64_t mantissa = 0;
    int digits = 0;
    int fraction = 0;
    bool after_dot = false;
    const char* p = first;
    for (; p != last && *p != 'e' && *p != 'E'; ++p) {
      if (*p == '.') {
        if (after_dot) {
          return false;
        }
        after_dot = true;
        continue;
      }
      if (*p < '0' || *p > '9' || ++digits > 15) {
        return false;
      }
      mantissa = mantissa * 10 + (*p - '0');
      if (after_dot) {
        ++fraction;
      }
    }
    // A mantissa needs a digit: "e5" and "1." are not numbers.
    if (digits == 0 || (after_dot && fraction == 0)) {
      return false;
    }
    int exponent = 0;
    if (p != last) {
      ++p;
      bool exponent_negative = false;
      if (p != last && (*p == '+' || *p == '-')) {
        exponent_negative = *p == '-';
        ++p;
      }
      if (p == last) {
        return false;
      }
      for (; p != last; ++p) {
        if (*p < '0' || *p > '9' || exponent > 100) {
          return false;
        }
        exponent = exponent * 10 + (*p - '0');
      }
      if (exponent_negative) {
        exponent = -exponent;
      }
    }
    const int power = exponent - fraction;
    if (power < -22 || power > 22) {
      return false;
    }
    double scale = 1.0;
    for (int i = 0; i < (power < 0 ? -power : power); ++i) {
      scale *= 10.0;
    }
    const double magnitude = static_cast<double>(mantissa);
    out = power < 0 ? magnitude / scale : magnitude * scale;
    if (negative) {
      out = -out;
    }
    return true;
  }

  // nsan cannot follow from_chars into a double: a value past int64 has no
  // shadow, so its store is reported. Only this parse is exempt from the check.
#if defined(__clang__)
  __attribute__((no_sanitize("numerical")))
#endif
  bool ParseNumber(JsonValue& value) {
    const std::size_t start = position_;
    while (position_ < text_.size() &&
           (text_[position_] == '-' || text_[position_] == '+' ||
            text_[position_] == '.' || text_[position_] == 'e' ||
            text_[position_] == 'E' ||
            (text_[position_] >= '0' && text_[position_] <= '9'))) {
      position_++;
    }
    if (position_ == start) {
      return Fail("a value was expected");
    }
    const char* const first = text_.data() + start;
    const char* const last = text_.data() + position_;
    // A whole number is read as an integer and converted: from_chars writes a
    // double without the shadow nsan keeps for it, so the conversion is the
    // store nsan can follow. Profiles hold whole numbers only.
    std::int64_t whole = 0;
    const auto [whole_end, whole_error] = std::from_chars(first, last, whole);
    if (whole_error == std::errc{} && whole_end == last) {
      value.kind = JsonValue::Kind::Number;
      value.number = static_cast<double>(whole);
      return true;
    }
    double parsed = 0;
    if (ParseShortDecimal(first, last, parsed)) {
      value.kind = JsonValue::Kind::Number;
      value.number = parsed;
      return true;
    }
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last || !std::isfinite(parsed)) {
      position_ = start;
      return Fail("a number was expected");
    }
    value.kind = JsonValue::Kind::Number;
    value.number = parsed;
    return true;
  }

  // Appends code point to out as UTF-8.
  static void AppendUtf8(std::string& out, char32_t code_point) {
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code_point >> 6U)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code_point >> 12U)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code_point >> 18U)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    }
  }

  // Reads four hexadecimal digits.
  bool ParseHex4(char32_t& unit) {
    if (position_ + 4 > text_.size()) {
      return Fail("a \\u escape is cut");
    }
    unit = 0;
    for (int digit = 0; digit < 4; digit++) {
      const char c = text_[position_++];
      unit <<= 4U;
      if (c >= '0' && c <= '9') {
        unit |= static_cast<char32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        unit |= static_cast<char32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        unit |= static_cast<char32_t>(c - 'A' + 10);
      } else {
        return Fail("a \\u escape holds a non-hexadecimal digit");
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    if (!Consume('"')) {
      return Fail("a string was expected");
    }
    out.clear();
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return Fail("a control character sits in a string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) {
        break;
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escape);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          char32_t unit = 0;
          if (!ParseHex4(unit)) {
            return false;
          }
          if (unit >= 0xD800 && unit <= 0xDBFF) {
            char32_t low = 0;
            if (!Consume('\\') || !Consume('u') || !ParseHex4(low) ||
                low < 0xDC00 || low > 0xDFFF) {
              return Fail("a high surrogate has no low surrogate");
            }
            unit = 0x10000 + ((unit - 0xD800) << 10U) + (low - 0xDC00);
          } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return Fail("a low surrogate stands alone");
          }
          AppendUtf8(out, unit);
          break;
        }
        default:
          return Fail("an unknown escape sits in a string");
      }
    }
    return Fail("a string is not closed");
  }

  bool ParseArray(JsonValue& value, std::size_t depth) {
    if (depth >= max_depth) {
      return Fail("the document nests too deep");
    }
    value.kind = JsonValue::Kind::Array;
    position_++;
    SkipSpace();
    if (Consume(']')) {
      return true;
    }
    for (;;) {
      SkipSpace();
      JsonValue item;
      if (!ParseValue(item, depth + 1)) {
        return false;
      }
      value.items.push_back(std::move(item));
      SkipSpace();
      if (Consume(']')) {
        return true;
      }
      if (!Consume(',')) {
        return Fail("a comma or a closing bracket was expected");
      }
    }
  }

  bool ParseObject(JsonValue& value, std::size_t depth) {
    if (depth >= max_depth) {
      return Fail("the document nests too deep");
    }
    value.kind = JsonValue::Kind::Object;
    position_++;
    SkipSpace();
    if (Consume('}')) {
      return true;
    }
    for (;;) {
      SkipSpace();
      std::string key;
      if (!ParseString(key)) {
        return false;
      }
      if (value.Find(key) != nullptr) {
        return Fail("the key \"" + key + "\" appears twice");
      }
      SkipSpace();
      if (!Consume(':')) {
        return Fail("a colon was expected");
      }
      SkipSpace();
      JsonValue member;
      if (!ParseValue(member, depth + 1)) {
        return false;
      }
      value.members.push_back({std::move(key), std::move(member)});
      SkipSpace();
      if (Consume('}')) {
        return true;
      }
      if (!Consume(',')) {
        return Fail("a comma or a closing brace was expected");
      }
    }
  }

  std::string_view text_;
  std::size_t position_{0};
  std::string error_;
};

// Appends text to out as a JSON string body.
void AppendEscaped(std::string& out, std::string_view text) {
  constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (const char c : text) {
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
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out.push_back(hex.at(static_cast<unsigned char>(c) >> 4U));
          out.push_back(hex.at(static_cast<unsigned char>(c) & 0xFU));
        } else {
          out.push_back(c);
        }
    }
  }
}

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
  return Parser(text).Run(error);
}

void JsonWriter::Separate() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (levels_.empty()) {
    return;
  }
  Level& level = levels_.back();
  if (!level.empty) {
    out_.push_back(',');
  }
  level.empty = false;
  out_.push_back('\n');
  out_.append(levels_.size() * 2, ' ');
}

void JsonWriter::BeginObject() {
  Separate();
  out_.push_back('{');
  levels_.push_back({.object = true, .empty = true});
}

void JsonWriter::EndObject() {
  const bool empty = levels_.back().empty;
  levels_.pop_back();
  if (!empty) {
    out_.push_back('\n');
    out_.append(levels_.size() * 2, ' ');
  }
  out_.push_back('}');
}

void JsonWriter::BeginArray() {
  Separate();
  out_.push_back('[');
  levels_.push_back({.object = false, .empty = true});
}

void JsonWriter::EndArray() {
  const bool empty = levels_.back().empty;
  levels_.pop_back();
  if (!empty) {
    out_.push_back('\n');
    out_.append(levels_.size() * 2, ' ');
  }
  out_.push_back(']');
}

void JsonWriter::Key(std::string_view key) {
  Separate();
  out_.push_back('"');
  AppendEscaped(out_, key);
  out_ += "\": ";
  after_key_ = true;
}

void JsonWriter::String(std::string_view value) {
  Separate();
  out_.push_back('"');
  AppendEscaped(out_, value);
  out_.push_back('"');
}

void JsonWriter::Number(std::uint64_t value) {
  Separate();
  out_ += std::to_string(value);
}

void JsonWriter::Number(std::int64_t value) {
  Separate();
  out_ += std::to_string(value);
}

void JsonWriter::Number(double value) {
  Separate();
  std::array<char, 32> digits{};
  const auto [end, error] =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  out_.append(digits.data(), error == std::errc{} ? end : digits.data());
  if (error != std::errc{}) {
    out_ += "0";
  }
}

void JsonWriter::Bool(bool value) {
  Separate();
  out_ += value ? "true" : "false";
}

void JsonWriter::Null() {
  Separate();
  out_ += "null";
}

std::string JsonWriter::Take() {
  out_.push_back('\n');
  return std::move(out_);
}

}  // namespace NtfsDevice
