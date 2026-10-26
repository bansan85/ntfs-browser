#include "name-support.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// An ExoticName from the UTF-16 units of the record and the UTF-8 expected.
ExoticName Make(std::string_view label, std::u16string_view units,
                std::string utf8) {
  return {
      .label = label, .name = Utf::Utf16ToWide(units), .utf8 = std::move(utf8)};
}

}  // namespace

std::wstring FillerName(std::size_t units, wchar_t fill, wchar_t tag) {
  std::wstring name(units, fill);
  if (units != 0) {
    name.back() = tag;
  }
  return name;
}

const std::vector<ExoticName>& ExoticNames() {
  // The name with a NUL has its length given: a literal is read as a C
  // string. Where a hex digit follows a hex escape, the literal is split.
  static const std::vector<ExoticName> names{
      Make("character past the BMP",
           u"a\xD83D\xDE00"
           u"b",
           "a\xF0\x9F\x98\x80"
           "b"),
      Make("high surrogate alone",
           u"x\xD800"
           u"y",
           "x\xED\xA0\x80"
           "y"),
      Make("low surrogate alone",
           u"x\xDC00"
           u"y",
           "x\xED\xB0\x80"
           "y"),
      Make("low surrogate before a high one", u"\xDE00\xD83D",
           "\xED\xB8\x80\xED\xA0\xBD"),
      Make("high surrogate at the end", u"end\xDBFF", "end\xED\xAF\xBF"),
      Make("combining mark", u"e\x0301", "e\xCC\x81"),
      Make("zero-width characters",
           u"a\x200B\x200D\xFEFF"
           u"z",
           "a\xE2\x80\x8B\xE2\x80\x8D\xEF\xBB\xBF"
           "z"),
      Make("bidirectional controls",
           u"a\x202E\x2066\x200F"
           u"z",
           "a\xE2\x80\xAE\xE2\x81\xA6\xE2\x80\x8F"
           "z"),
      Make("noncharacters", u"\xFFFF\xFFFE\xFDD0",
           "\xEF\xBF\xBF\xEF\xBF\xBE\xEF\xB7\x90"),
      Make("embedded U+0000", std::u16string_view(u"a\0b", 3),
           std::string("a\0b", 3)),
      Make("separators", u"a\\b/c:d", "a\\b/c:d"),
      Make("trailing dot", u"dot.", "dot."),
      Make("trailing space", u"space ", "space "),
  };
  return names;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
