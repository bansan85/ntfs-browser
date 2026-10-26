#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "name-support.h"
#include "text-convert.h"
#include "utf/utf.h"

using NtfsDevice::PathFromWide;
using NtfsDevice::WideFromPath;
using NtfsDevice::WideToUtf8;

namespace {

// A wide string holding the single code unit unit, which may be a surrogate.
std::wstring Unit(unsigned unit) {
  return std::wstring(1, static_cast<wchar_t>(unit));
}

}  // namespace

TEST_CASE("WideToUtf8 encodes ASCII and accented names",
          "[device-access][text-convert]") {
  CHECK(WideToUtf8(L"") == "");
  CHECK(WideToUtf8(L"report.txt") == "report.txt");
  CHECK(WideToUtf8(L"café") == "caf\xC3\xA9");
  CHECK(WideToUtf8(L"€") == "\xE2\x82\xAC");
}

TEST_CASE("WideToUtf8 encodes a character outside the BMP as four bytes",
          "[device-access][text-convert]") {
  CHECK(WideToUtf8(L"\U0001F600") == "\xF0\x9F\x98\x80");
  CHECK(WideToUtf8(L"a\U0001F600b") ==
        "a\xF0\x9F\x98\x80"
        "b");
}

TEST_CASE("WideToUtf8 encodes a lone surrogate in its 3-byte WTF-8 form",
          "[device-access][text-convert]") {
  CHECK(WideToUtf8(Unit(0xD800)) == "\xED\xA0\x80");
  CHECK(WideToUtf8(Unit(0xDC80)) == "\xED\xB2\x80");
  CHECK(WideToUtf8(Unit(0xDC80) + L"a") ==
        "\xED\xB2\x80"
        "a");
}

TEST_CASE("PathFromWide and WideFromPath round-trip a non-ASCII name",
          "[device-access][text-convert]") {
  const std::wstring name = L"café-\U0001F600";
  CHECK(WideFromPath(PathFromWide(name)) == name);
}

#ifndef _WIN32

using NtfsDevice::Utf8ToWide;

TEST_CASE("Utf8ToWide decodes a lone surrogate's WTF-8 form exactly",
          "[device-access][text-convert]") {
  CHECK(Utf8ToWide("\xED\xA0\x80") == Unit(0xD800));
  CHECK(Utf8ToWide("\xED\xB2\x80") == Unit(0xDC80));
  CHECK(Utf8ToWide("\xED\xA0\x80"
                   "a") == Unit(0xD800) + L"a");
}

TEST_CASE("Utf8ToWide decodes accented and astral names unchanged",
          "[device-access][text-convert]") {
  CHECK(Utf8ToWide("caf\xC3\xA9") == L"café");
  CHECK(Utf8ToWide("\xF0\x9F\x98\x80") == L"\U0001F600");
  CHECK(Utf8ToWide("e\xCC\x81") == L"é");
}

TEST_CASE("Utf8ToWide skips an invalid byte instead of failing",
          "[device-access][text-convert]") {
  CHECK(Utf8ToWide("a\xFF"
                   "b") == L"ab");
  CHECK(Utf8ToWide("a\xC3") == L"a");
}

TEST_CASE("Utf8ToWide undoes WideToUtf8", "[device-access][text-convert]") {
  const std::wstring names[] = {
      L"plain",     L"café",      L"\U0001F600",
      Unit(0xD800), Unit(0xDC80), L"x" + Unit(0xD800) + L"y",
  };
  for (const std::wstring& name : names) {
    CHECK(Utf8ToWide(WideToUtf8(name)) == name);
  }
}

TEST_CASE("Utf8ToWide decodes every exotic name back",
          "[device-access][text-convert]") {
  for (const NTFS_BROWSER_NAMESPACE::Tests::ExoticName& exotic :
       NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    INFO(exotic.label);
    CHECK(Utf8ToWide(exotic.utf8) == exotic.name);
  }
}

#endif  // !_WIN32

TEST_CASE("WideToUtf8 writes every exotic name as its UTF-8",
          "[device-access][text-convert]") {
  for (const NTFS_BROWSER_NAMESPACE::Tests::ExoticName& exotic :
       NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    INFO(exotic.label);
    CHECK(WideToUtf8(exotic.name) == exotic.utf8);
  }
}

TEST_CASE("AppendAsUtf8 appends what WideToUtf8 returns",
          "[device-access][text-convert]") {
  for (const NTFS_BROWSER_NAMESPACE::Tests::ExoticName& exotic :
       NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    INFO(exotic.label);
    std::string out = "before:";
    NtfsDevice::AppendAsUtf8(out, exotic.name);
    CHECK(out == "before:" + exotic.utf8);
  }
}

TEST_CASE("WideToUtf8 and the library's conversion agree on every name",
          "[device-access][text-convert]") {
  for (const NTFS_BROWSER_NAMESPACE::Tests::ExoticName& exotic :
       NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    INFO(exotic.label);
    CHECK(WideToUtf8(exotic.name) ==
          NTFS_BROWSER_NAMESPACE::Utf::WideToUtf8(exotic.name));
  }
}

TEST_CASE("WideToUtf8 keeps a noncharacter and a U+0000 apart from U+FFFD",
          "[device-access][text-convert]") {
  CHECK(WideToUtf8(Unit(0xFFFF)) == "\xEF\xBF\xBF");
  CHECK(WideToUtf8(Unit(0xFFFE)) == "\xEF\xBF\xBE");
  CHECK(WideToUtf8(Unit(0xFFFD)) == "\xEF\xBF\xBD");
  CHECK(WideToUtf8(Unit(0)) == std::string(1, '\0'));
}

TEST_CASE("PathFromWide and WideFromPath round-trip an astral character",
          "[device-access][text-convert]") {
  const std::wstring name = L"\U0001F600" + Unit(0xE000);
  CHECK(WideFromPath(PathFromWide(name)) == name);
}
