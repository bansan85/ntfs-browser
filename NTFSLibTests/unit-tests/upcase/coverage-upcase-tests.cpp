#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "upcase/upcase.h"

namespace {

// Code point past U+10FFFF. No name can hold it, but a std::wstring can.
constexpr wchar_t beyond_unicode = static_cast<wchar_t>(0x110000);

}  // namespace

TEST_CASE("a code point past U+10FFFF compares as the replacement character",
          "[cov-attrs]") {
  if constexpr (sizeof(wchar_t) > sizeof(char16_t)) {
    const std::wstring beyond(1, beyond_unicode);
    CHECK(NTFS_BROWSER_NAMESPACE::UpCase::Table::BuiltIn().Compare(beyond,
                                                                   L"a") > 0);
  } else {
    SKIP("wchar_t is 16 bits here: no code point past the BMP exists");
  }
}
