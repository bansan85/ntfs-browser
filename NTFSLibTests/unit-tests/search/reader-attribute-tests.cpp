#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "reader-support.h"
#include "scan-support.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::RowNamed;
using TestSearch::TempImage;
using Bytes = std::vector<std::uint8_t>;

namespace {

// The handle of the root record of the fixtures that replace it. Sequence 0
// takes whatever the record holds.
constexpr Lib::Handle root_handle{.record = 5};

// A volume whose root record is a file with an unnamed $DATA stream of 100
// bytes and a named one, "side", of 50, none of them encrypted.
std::vector<BYTE> TwoStreamImage() {
  TestFake::FakeEncryptedFile file;
  file.streams.push_back({.runs = {{40, 1}},
                          .cluster_bytes = std::vector<BYTE>(100, 'A'),
                          .real_size = 100,
                          .flagged_encrypted = false});
  file.streams.push_back({.name = L"side",
                          .runs = {{41, 1}},
                          .cluster_bytes = std::vector<BYTE>(50, 'B'),
                          .real_size = 50,
                          .flagged_encrypted = false});
  file.other_logged_stream_name = L"$TXF_DATA";
  return TestFake::BuildFakeNtfsImageWithEncryptedFile(file);
}

// Four nodes: a file with the times, the attribute bits and the size set.
std::vector<BYTE> TimedImage() {
  std::vector<TestFake::FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"timed.txt"}},
                   .data_size = 12,
                   .attributes = 0x22,
                   .times = {.created = 1'000'000'000ULL,
                             .modified = 2'000'000'000ULL,
                             .mft_changed = 3'000'000'000ULL,
                             .accessed = 4'000'000'000ULL}});
  return TestFake::BuildFakeNtfsImageWithNameTree(nodes);
}

}  // namespace

TEST_CASE("A Reader gives the standard information of a file",
          "[search][reader]") {
  const TempImage image(TimedImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"timed.txt");
  REQUIRE(row.name == u"timed.txt");

  const auto info = reader.GetStandardInformation(row.handle);

  REQUIRE(info);
  CHECK(info.value.created.ticks == 1'000'000'000ULL);
  CHECK(info.value.modified.ticks == 2'000'000'000ULL);
  CHECK(info.value.mft_changed.ticks == 3'000'000'000ULL);
  CHECK(info.value.accessed.ticks == 4'000'000'000ULL);
  CHECK((info.value.attributes & Lib::Attribute::hidden) != 0);
  CHECK((info.value.attributes & Lib::Attribute::archive) != 0);
  CHECK(info.value.created.ticks == row.created);
}

TEST_CASE("A Reader gives every name of a file with its parent and times",
          "[search][reader]") {
  const TempImage image(TestSearch::HardLinkImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});

  SECTION("a hard link") {
    const auto row = RowNamed(rows, u"one.txt");

    const auto names = reader.GetFileNames(row.handle);

    REQUIRE(names);
    REQUIRE(names.value.size() == 2);
    CHECK(names.value[0].name == u"one.txt");
    CHECK(names.value[0].parent_record == 5);
    CHECK(names.value[1].name == u"two.txt");
    CHECK(names.value[1].parent_record == TestSearch::Record(0));
    CHECK(names.value[1].index == 1);
    CHECK(names.value[0].name_space == Lib::NameSpace::Win32);
  }

  SECTION("a long name and its DOS alias") {
    const auto row = RowNamed(rows, u"LongName.txt");

    const auto names = reader.GetFileNames(row.handle);

    REQUIRE(names);
    REQUIRE(names.value.size() == 2);
    const bool has_alias =
        std::ranges::any_of(names.value, [](const Lib::FileName& name) {
          return name.name == u"LONGNA~1.TXT" &&
                 name.name_space == Lib::NameSpace::Dos;
        });
    CHECK(has_alias);
  }
}

TEST_CASE("A Reader lists the data streams of a file with their sizes",
          "[search][reader]") {
  const TempImage image(TwoStreamImage());
  Lib::Reader reader({.target = image.Target()});

  const auto streams = reader.GetStreams(root_handle);

  INFO(static_cast<int>(streams.error));
  REQUIRE(streams);
  REQUIRE(streams.value.size() == 2);
  CHECK(streams.value[0].name.empty());
  CHECK(streams.value[0].size == 100);
  CHECK_FALSE(streams.value[0].resident);
  CHECK_FALSE(streams.value[0].encrypted);
  CHECK(streams.value[1].name == u"side");
  CHECK(streams.value[1].size == 50);
}

TEST_CASE("A Reader reads any named data stream", "[search][reader]") {
  const TempImage image(TwoStreamImage());
  Lib::Reader reader({.target = image.Target()});

  Bytes unnamed(100, 0xEE);
  Bytes side(60, 0xEE);
  const auto first = reader.Read(root_handle, u"", 0, unnamed);
  const auto second = reader.Read(root_handle, u"side", 0, side);

  REQUIRE(first);
  CHECK(first.value == 100);
  CHECK(unnamed == Bytes(100, 'A'));
  REQUIRE(second);
  CHECK(second.value == 50);
  CHECK(Bytes(side.begin(), side.begin() + 50) == Bytes(50, 'B'));
  CHECK(side[50] == 0xEE);
}

TEST_CASE("A Reader gives the raw bytes of every other attribute",
          "[search][reader]") {
  const TempImage image(TwoStreamImage());
  Lib::Reader reader({.target = image.Target()});

  const auto attributes = reader.GetAttributes(root_handle);

  REQUIRE(attributes);
  const auto find = [&](Lib::AttributeType type, const std::u16string& name) {
    return std::ranges::find_if(
        attributes.value, [&](const Lib::AttributeInfo& attribute) {
          return attribute.type == type && attribute.name == name;
        });
  };
  const auto standard = find(0x10, u"");
  REQUIRE(standard != attributes.value.end());
  CHECK(standard->resident);
  const auto data = find(0x80, u"side");
  REQUIRE(data != attributes.value.end());
  CHECK(data->size == 50);
  CHECK(find(0x100, u"$TXF_DATA") != attributes.value.end());

  SECTION("by type") {
    Bytes bytes(standard->size, 0xEE);

    const auto read =
        reader.ReadAttribute(root_handle, {.type = 0x10}, 0, bytes);

    REQUIRE(read);
    CHECK(read.value == standard->size);
    CHECK(bytes.size() >= 8);
    const auto info = reader.GetStandardInformation(root_handle);
    REQUIRE(info);
    std::uint64_t created = 0;
    std::memcpy(&created, bytes.data(), sizeof(created));
    CHECK(created == info.value.created.ticks);
  }

  SECTION("by type and name") {
    Bytes bytes(80, 0xEE);

    const auto read = reader.ReadAttribute(
        root_handle, {.type = 0x80, .name = u"side"}, 10, bytes);

    REQUIRE(read);
    CHECK(read.value == 40);
    CHECK(Bytes(bytes.begin(), bytes.begin() + 40) == Bytes(40, 'B'));
  }

  SECTION("by type, name and instance") {
    Bytes bytes(4, 0xEE);

    const auto right = reader.ReadAttribute(
        root_handle,
        {.type = 0x80, .name = u"side", .instance = data->instance}, 0, bytes);
    const auto wrong = reader.ReadAttribute(
        root_handle,
        {.type = 0x80,
         .name = u"side",
         .instance = static_cast<std::uint16_t>(data->instance + 100)},
        0, bytes);

    CHECK(right);
    CHECK(wrong.error == Lib::ReadError::NoSuchAttribute);
  }

  SECTION("an attribute the record does not have") {
    Bytes bytes(4, 0xEE);

    const auto read =
        reader.ReadAttribute(root_handle, {.type = 0x40}, 0, bytes);

    CHECK(read.error == Lib::ReadError::NoSuchAttribute);
    CHECK(bytes == Bytes(4, 0xEE));
  }
}

TEST_CASE("A Reader fails cleanly on a damaged record", "[search][reader]") {
  (void)TestSupport::TakeCapturedLog();

  SECTION("a root record that does not parse") {
    const TempImage image(TestFake::BuildFakeNtfsImageWithCorruptRootRecord());
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());
    Bytes bytes(4, 0xEE);

    CHECK_FALSE(reader.Check(root_handle));
    CHECK_FALSE(reader.GetFileNames(root_handle));
    CHECK_FALSE(reader.GetStreams(root_handle));
    CHECK_FALSE(reader.Read(root_handle, u"", 0, bytes));
    CHECK(bytes == Bytes(4, 0xEE));
  }

  SECTION("a record whose attribute is cut short") {
    const TempImage image(
        TestFake::BuildFakeNtfsImageWithUndersizedAttribute());
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());
    Bytes bytes(4, 0xEE);

    const Lib::Handle handle{.record = TestFake::undersized_attr_record_idx};

    CHECK_FALSE(reader.Read(handle, u"", 0, bytes));
    CHECK(bytes == Bytes(4, 0xEE));
  }
}
