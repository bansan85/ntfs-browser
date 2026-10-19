#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "argv-wide.h"

using NtfsDevice::ArgumentPointers;
using NtfsDevice::ArgumentsAsWide;

TEST_CASE("ArgumentsAsWide keeps the order and decodes UTF-8",
          "[device-access][argv-wide]") {
  char program[] = "ntfsdir";
  char option[] = "--log=console:off";
  char accented[] = "/dev/sda1/caf\xC3\xA9";
  char* argv[] = {program, option, accented};

  const std::vector<std::wstring> wide = ArgumentsAsWide(3, argv);

  REQUIRE(wide.size() == 3);
  CHECK(wide[0] == L"ntfsdir");
  CHECK(wide[1] == L"--log=console:off");
  CHECK(wide[2] == L"/dev/sda1/café");
}

TEST_CASE("ArgumentPointers points at each argument in order",
          "[device-access][argv-wide]") {
  std::vector<std::wstring> arguments = {L"first", L"second"};

  const std::vector<wchar_t*> pointers = ArgumentPointers(arguments);

  REQUIRE(pointers.size() == 2);
  CHECK(std::wstring(pointers[0]) == L"first");
  CHECK(std::wstring(pointers[1]) == L"second");
}
