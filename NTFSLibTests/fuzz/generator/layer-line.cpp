#include "layer-line.h"

#include <ntfs-browser/win-types.h>

#include <charconv>
#include <cstddef>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "hex-bytes.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The base of a number written after "0x".
constexpr int hex_base = 16;

// The base of a number written without a prefix.
constexpr int decimal_base = 10;

}  // namespace

LayerLine::LayerLine(std::string_view text) : source_(text) {
  std::istringstream words{source_};
  std::string word;
  words >> kind_;
  while (words >> word) {
    const std::size_t equals = word.find('=');
    if (equals == std::string::npos) {
      positional_.push_back(word);
    } else if (!fields_.emplace(word.substr(0, equals), word.substr(equals + 1))
                    .second) {
      Fail("a field repeats");
    }
  }
}

void LayerLine::Fail(std::string_view what) const {
  throw std::runtime_error(std::string(what) + ": " + source_);
}

const std::string& LayerLine::Kind() const { return kind_; }

const std::string& LayerLine::Source() const { return source_; }

bool LayerLine::Has(std::string_view name) const {
  return fields_.contains(name);
}

const std::string& LayerLine::Text(std::string_view name) const {
  const auto found = fields_.find(name);
  if (found == fields_.end()) {
    Fail("missing field " + std::string(name));
  }
  return found->second;
}

ULONGLONG LayerLine::Parse(std::string_view word,
                           std::string_view value) const {
  int base = decimal_base;
  if (value.starts_with("0x")) {
    base = hex_base;
    value.remove_prefix(2);
  }
  ULONGLONG number = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), number, base);
  if (error != std::errc{} || end != value.data() + value.size()) {
    Fail("bad number in " + std::string(word));
  }
  return number;
}

ULONGLONG LayerLine::Number(std::string_view name, ULONGLONG fallback) const {
  const auto found = fields_.find(name);
  return found == fields_.end() ? fallback : Parse(name, found->second);
}

ULONGLONG LayerLine::Number(std::string_view name) const {
  return Parse(name, Text(name));
}

std::vector<BYTE> LayerLine::Bytes(std::string_view name) const {
  const auto found = fields_.find(name);
  return found == fields_.end() ? std::vector<BYTE>{}
                                : HexBytes::Decode(found->second);
}

ULONGLONG LayerLine::Positional(std::size_t index) const {
  if (index >= positional_.size()) {
    Fail("a word is missing");
  }
  return Parse("a word", positional_[index]);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
