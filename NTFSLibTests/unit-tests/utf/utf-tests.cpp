// Tests for the UTF-16 to UTF-8 decoder (src/utf/utf.h) the log call sites
// run on-disk names through. Every name in the rest of the suite is
// ASCII, so nothing else reaches the multi-byte or surrogate branches.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "utf/utf.h"

namespace {

// One decoder input and the UTF-8 bytes it must produce.
struct Case {
  const char* name;
  std::wstring input;
  std::string expected;
};

}  // namespace

TEST_CASE("WideToUtf8 encodes every UTF-16 form", "[utf]") {
  // U+FFFD REPLACEMENT CHARACTER, what an unpaired surrogate becomes.
  const std::string replacement = "\xEF\xBF\xBD";

  const std::vector<Case> cases{
      {"ASCII stays one byte", std::wstring{L'A'}, "A"},
      {"U+00E9 is two bytes", std::wstring{static_cast<wchar_t>(0x00E9)},
       "\xC3\xA9"},
      {"U+20AC is three bytes", std::wstring{static_cast<wchar_t>(0x20AC)},
       "\xE2\x82\xAC"},
      {"a surrogate pair is one four-byte code point",
       std::wstring{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)},
       "\xF0\x9F\x98\x80"},
      {"a lone high surrogate is replaced",
       std::wstring{static_cast<wchar_t>(0xD83D)}, replacement},
      {"a lone low surrogate is replaced",
       std::wstring{static_cast<wchar_t>(0xDE00)}, replacement},
      {"a high surrogate not followed by a low one is replaced on its own",
       std::wstring{static_cast<wchar_t>(0xD83D), L'A'}, replacement + "A"},
  };

  for (const Case& test_case : cases) {
    INFO(test_case.name);
    CHECK(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(test_case.input) ==
          test_case.expected);
  }
}

TEST_CASE("WideToUtf8 replaces a unit that no code point can be", "[utf]") {
  // U+FFFD REPLACEMENT CHARACTER.
  const std::string replacement = "\xEF\xBF\xBD";
  // A high surrogate followed by a unit above the low surrogates.
  const std::wstring above_low{static_cast<wchar_t>(0xD83D),
                               static_cast<wchar_t>(0xE000)};

  CHECK(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(above_low) ==
        replacement + "\xEE\x80\x80");

  if constexpr (sizeof(wchar_t) > sizeof(char16_t)) {
    // First value past U+10FFFF: only a 32-bit wchar_t can hold it.
    const std::wstring past_unicode{static_cast<wchar_t>(0x110000)};
    CHECK(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(past_unicode) == replacement);
  }
}

TEST_CASE("Utf16ToWide keeps what is not a surrogate pair", "[utf]") {
  using NTFS_BROWSER_NAMESPACE::Utf::Utf16ToWide;

  CHECK(Utf16ToWide(u"Aé") == L"Aé");

  if constexpr (sizeof(wchar_t) > sizeof(char16_t)) {
    SECTION("a pair becomes one code point") {
      CHECK(Utf16ToWide(u"\xD83D\xDE00") ==
            std::wstring{static_cast<wchar_t>(0x1F600)});
    }
    SECTION("a high surrogate at the end stays as it is") {
      CHECK(Utf16ToWide(u"A\xD83D") ==
            std::wstring{L'A', static_cast<wchar_t>(0xD83D)});
    }
    SECTION("a high surrogate before a unit past the low surrogates") {
      CHECK(Utf16ToWide(u"\xD83D\xE000") ==
            std::wstring{static_cast<wchar_t>(0xD83D),
                         static_cast<wchar_t>(0xE000)});
    }
  } else {
    CHECK(Utf16ToWide(u"\xD83D\xDE00") ==
          std::wstring{static_cast<wchar_t>(0xD83D),
                       static_cast<wchar_t>(0xDE00)});
  }
}
