#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader-types.h>
#include <ntfs-browser/types.h>

#include "search/attribute-parse.h"

namespace Lib = NTFS_BROWSER_NAMESPACE;

namespace {

// Offsets in a $FILE_NAME of the name length, the name space and the name.
constexpr std::size_t length_offset = 0x40;
constexpr std::size_t space_offset = 0x41;
constexpr std::size_t name_offset = 66;

// A $FILE_NAME of a one unit name "x" in the given name space byte.
std::vector<std::uint8_t> FileNameBytes(std::uint8_t name_space) {
  std::vector<std::uint8_t> bytes(name_offset + 2, 0);
  bytes.at(length_offset) = 1;
  bytes.at(space_offset) = name_space;
  bytes.at(name_offset) = 'x';
  return bytes;
}

}  // namespace

TEST_CASE("a $STANDARD_INFORMATION shorter than 48 bytes is refused",
          "[search][reader][attribute-parse]") {
  const std::vector<std::uint8_t> bytes(47, 0);

  CHECK_FALSE(Lib::ParseStandardInformation(bytes));
}

TEST_CASE("a $STANDARD_INFORMATION of 48 bytes has no extended fields",
          "[search][reader][attribute-parse]") {
  const std::vector<std::uint8_t> bytes(48, 0);

  const auto info = Lib::ParseStandardInformation(bytes);

  REQUIRE(info);
  CHECK_FALSE(info->has_extended);
}

TEST_CASE("a $STANDARD_INFORMATION of 72 bytes has its extended fields",
          "[search][reader][attribute-parse]") {
  std::vector<std::uint8_t> bytes(72, 0);
  const std::uint32_t owner = 77;
  std::memcpy(bytes.data() + 0x30, &owner, sizeof(owner));

  const auto info = Lib::ParseStandardInformation(bytes);

  REQUIRE(info);
  CHECK(info->has_extended);
  CHECK(info->owner_id == 77);
}

TEST_CASE("a $FILE_NAME is refused when it is cut or has no name",
          "[search][reader][attribute-parse]") {
  SECTION("shorter than its fixed part") {
    const std::vector<std::uint8_t> bytes(65, 0);

    CHECK_FALSE(Lib::ParseFileName(bytes, 0));
  }

  SECTION("an empty name") {
    std::vector<std::uint8_t> bytes = FileNameBytes(1);
    bytes.at(length_offset) = 0;

    CHECK_FALSE(Lib::ParseFileName(bytes, 0));
  }

  SECTION("a name longer than the bytes") {
    std::vector<std::uint8_t> bytes = FileNameBytes(1);
    bytes.at(length_offset) = 2;

    CHECK_FALSE(Lib::ParseFileName(bytes, 0));
  }
}

TEST_CASE("a $FILE_NAME gives its name space",
          "[search][reader][attribute-parse]") {
  const auto space_of = [](std::uint8_t value) {
    const auto name = Lib::ParseFileName(FileNameBytes(value), 3);
    REQUIRE(name);
    CHECK(name->index == 3);
    CHECK(name->name == u"x");
    return name->name_space;
  };

  CHECK(space_of(0) == Lib::NameSpace::Posix);
  CHECK(space_of(1) == Lib::NameSpace::Win32);
  CHECK(space_of(2) == Lib::NameSpace::Dos);
  CHECK(space_of(3) == Lib::NameSpace::Win32AndDos);
  CHECK(space_of(9) == Lib::NameSpace::Posix);
}

TEST_CASE("a $FILE_NAME moves its directory bit to the directory attribute",
          "[search][reader][attribute-parse]") {
  std::vector<std::uint8_t> bytes = FileNameBytes(1);
  const std::uint32_t flags = 0x10000000 | 0x20;
  std::memcpy(bytes.data() + 0x38, &flags, sizeof(flags));

  const auto name = Lib::ParseFileName(bytes, 0);

  REQUIRE(name);
  CHECK((name->attributes & Lib::Attribute::directory) != 0);
  CHECK((name->attributes & Lib::Attribute::archive) != 0);
}
