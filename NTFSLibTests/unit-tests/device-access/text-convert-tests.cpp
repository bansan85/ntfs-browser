#include <filesystem>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "text-convert.h"

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

#endif  // !_WIN32
