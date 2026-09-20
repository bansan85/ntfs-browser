// Tests for the UTF-16 to UTF-8 decoder (src/utf.h) the log call sites
// run on-disk names through. Every name in the rest of the suite is
// ASCII, so nothing else reaches the multi-byte or surrogate branches.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "utf.h"

namespace
{

// One decoder input and the UTF-8 bytes it must produce.
struct Case
{
  const char* name;
  std::wstring input;
  std::string expected;
};

}  // namespace

TEST_CASE("WideToUtf8 encodes every UTF-16 form", "[utf]")
{
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

  for (const Case& testCase : cases)
  {
    INFO(testCase.name);
    CHECK(NtfsBrowser::WideToUtf8(testCase.input) == testCase.expected);
  }
}
