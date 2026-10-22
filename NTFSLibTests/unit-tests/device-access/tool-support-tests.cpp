#include <cstdlib>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

#include "tool-support.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
using NtfsDevice::IsLogOption;
using NtfsDevice::ParseLogOption;

TEST_CASE("IsLogOption recognises the --log= prefix only",
          "[device-access][tool-support]") {
  CHECK(IsLogOption(L"--log=console:off"));
  CHECK(IsLogOption(L"--log="));
  CHECK_FALSE(IsLogOption(L"--deleted"));
  CHECK_FALSE(IsLogOption(L"/dev/sda1"));
  CHECK_FALSE(IsLogOption(L""));
}

TEST_CASE("ParseLogOption fills the named target and rejects a bad level",
          "[device-access][tool-support]") {
  Log::Config config;
  REQUIRE(ParseLogOption(L"--log=console:debug", config));
  CHECK(config.console_level == Log::Level::Debug);

  Log::Config untouched;
  CHECK_FALSE(ParseLogOption(L"--log=console:bogus", untouched));
  CHECK(untouched.console_level == Log::Config{}.console_level);
}

TEST_CASE("ParseLogOption keeps a file path whole",
          "[device-access][tool-support]") {
  Log::Config config;
  REQUIRE(ParseLogOption(L"--log=file:trace:out/ntfs.log", config));
  CHECK(config.file_level == Log::Level::Trace);
  CHECK(config.file_path == std::filesystem::path("out/ntfs.log"));
}

#ifndef _WIN32

  #include <cstdio>

using NtfsDevice::WriteText;

namespace {

// Everything WriteText writes for text.
std::string Written(const std::wstring& text) {
  char* buffer = nullptr;
  size_t size = 0;
  std::FILE* const stream = open_memstream(&buffer, &size);
  REQUIRE(stream != nullptr);
  WriteText(stream, text);
  std::fclose(stream);

  std::string content(buffer, size);
  std::free(buffer);
  return content;
}

}  // namespace

TEST_CASE("WriteText writes ASCII as it is", "[device-access][tool-support]") {
  CHECK(Written(L"Cannot find directory Docs") == "Cannot find directory Docs");
  CHECK(Written(L"").empty());
}

TEST_CASE("WriteText writes UTF-8 whatever the locale",
          "[device-access][tool-support]") {
  CHECK(Written(L"café") == "caf\xC3\xA9");
}

#endif  // !_WIN32
