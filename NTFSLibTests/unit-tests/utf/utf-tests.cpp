// Tests for the UTF-16 to UTF-8 decoder (src/utf/utf.h) the log call sites
// run on-disk names through. Every name in the rest of the suite is
// ASCII, so nothing else reaches the multi-byte or surrogate branches.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "name-support.h"
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
  const std::vector<Case> cases{
      {"ASCII stays one byte", std::wstring{L'A'}, "A"},
      {"U+00E9 is two bytes", std::wstring{static_cast<wchar_t>(0x00E9)},
       "\xC3\xA9"},
      {"U+20AC is three bytes", std::wstring{static_cast<wchar_t>(0x20AC)},
       "\xE2\x82\xAC"},
      {"a surrogate pair is one four-byte code point",
       std::wstring{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)},
       "\xF0\x9F\x98\x80"},
      {"a lone high surrogate keeps its 3-byte form",
       std::wstring{static_cast<wchar_t>(0xD83D)}, "\xED\xA0\xBD"},
      {"a lone low surrogate keeps its 3-byte form",
       std::wstring{static_cast<wchar_t>(0xDE00)}, "\xED\xB8\x80"},
      {"a high surrogate not followed by a low one stays on its own",
       std::wstring{static_cast<wchar_t>(0xD83D), L'A'},
       "\xED\xA0\xBD"
       "A"},
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
        "\xED\xA0\xBD\xEE\x80\x80");

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

TEST_CASE("WideToUtf8 writes every exotic name as its UTF-8", "[utf]") {
  for (const NTFS_BROWSER_NAMESPACE::Tests::ExoticName& exotic :
       NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    INFO(exotic.label);
    CHECK(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(exotic.name) == exotic.utf8);
  }
}

TEST_CASE("WideToUtf8 keeps names that differ by a surrogate apart", "[utf]") {
  using NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8;
  const std::string first =
      WideToUtf8(std::wstring{static_cast<wchar_t>(0xD800)});
  const std::string second =
      WideToUtf8(std::wstring{static_cast<wchar_t>(0xD801)});
  const std::string replacement =
      WideToUtf8(std::wstring{static_cast<wchar_t>(0xFFFD)});

  CHECK(first != second);
  CHECK(first != replacement);
  CHECK(second != replacement);
}

TEST_CASE("WideToUtf8 keeps a NUL and the text around it", "[utf]") {
  const std::wstring name{L'a', L'\0', L'b'};

  CHECK(NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(name) ==
        std::string("a\0b", 3));
}
