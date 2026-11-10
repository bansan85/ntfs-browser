#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/scripted-disk-reader.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/types.h>

#include "disk-reader.h"
#include "reader-support.h"
#include "scan-support.h"
#include "search/scan-probe.h"
#include "search/target.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::RowNamed;
using TestSearch::TempImage;

namespace {

// Puts the real readers back when it goes out of scope.
class FactoryGuard final {
 public:
  explicit FactoryGuard(Lib::ReaderFactory factory) {
    Lib::SetReaderFactoryForTests(std::move(factory));
  }

  FactoryGuard(const FactoryGuard&) = delete;
  FactoryGuard& operator=(const FactoryGuard&) = delete;
  FactoryGuard(FactoryGuard&&) = delete;
  FactoryGuard& operator=(FactoryGuard&&) = delete;

  ~FactoryGuard() { Lib::SetReaderFactoryForTests({}); }
};

// A reader over memory that throws an int, not an exception, when a read
// touches [first, last).
class IntThrowingReader final : public Lib::IDiskReader {
 public:
  IntThrowingReader(std::vector<BYTE> image, ULONGLONG first, ULONGLONG last)
      : inner_(std::move(image)), first_(first), last_(last) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    if (begin < last_ && begin + dest.size() > first_) {
      throw 42;
    }
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

 private:
  TestSupport::ScriptedDiskReader inner_;
  ULONGLONG first_;
  ULONGLONG last_;
};

// The byte range of the three clusters of data.bin in ContentImage().
struct DataRange {
  ULONGLONG first;
  ULONGLONG last;
};

DataRange DataBinRange(const std::vector<BYTE>& image) {
  const ULONGLONG clusters = image.size() / TestFake::fake_cluster_size;
  const ULONGLONG first = clusters - 6;
  return {.first = first * TestFake::fake_cluster_size,
          .last = (first + 3) * TestFake::fake_cluster_size};
}

// The handle of record, any sequence, any name.
Lib::Handle HandleOf(std::uint64_t record) { return {.record = record}; }

}  // namespace

TEST_CASE("A Reader turns a read that throws into a failed result",
          "[search][reader]") {
  const std::vector<BYTE> bytes = TestSearch::ContentImage();
  const TempImage image(bytes);
  const auto rows = TestSearch::ListImage(image);
  const auto row = RowNamed(rows, u"data.bin");
  REQUIRE(row.name == u"data.bin");
  const DataRange range = DataBinRange(bytes);
  std::vector<std::uint8_t> buffer(16);

  SECTION("an exception") {
    const FactoryGuard factory(
        [&](const Lib::TargetParts&) -> std::unique_ptr<Lib::IDiskReader> {
          auto reader =
              std::make_unique<TestSupport::ScriptedDiskReader>(bytes);
          reader->FaultRange(range.first, range.last,
                             TestSupport::ScriptedDiskReader::Fault::Throw);
          return reader;
        });
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());

    const auto read = reader.Read(row.handle, {}, 0, buffer);

    CHECK_FALSE(read);
    CHECK(read.error == Lib::ReadError::Unreadable);
  }

  SECTION("something that is not an exception") {
    const FactoryGuard factory(
        [&](const Lib::TargetParts&) -> std::unique_ptr<Lib::IDiskReader> {
          return std::make_unique<IntThrowingReader>(bytes, range.first,
                                                     range.last);
        });
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());

    const auto read = reader.Read(row.handle, {}, 0, buffer);

    CHECK_FALSE(read);
    CHECK(read.error == Lib::ReadError::Unreadable);
  }
}

TEST_CASE("A Reader whose reader factory throws stays closed",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());

  SECTION("an exception") {
    const FactoryGuard factory(
        [](const Lib::TargetParts&) -> std::unique_ptr<Lib::IDiskReader> {
          throw std::runtime_error("no reader");
        });

    const Lib::Reader reader({.target = image.Target()});

    CHECK_FALSE(reader.IsOpen());
  }

  SECTION("something that is not an exception") {
    const FactoryGuard factory(
        [](const Lib::TargetParts&) -> std::unique_ptr<Lib::IDiskReader> {
          throw 42;
        });

    const Lib::Reader reader({.target = image.Target()});

    CHECK_FALSE(reader.IsOpen());
  }
}

TEST_CASE("A Reader refuses a record that extends another record",
          "[search][reader]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithMftTreeAttributeList());
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  const auto checked =
      reader.Check(HandleOf(TestFake::mft_tree_list_high_ext_idx));

  CHECK_FALSE(checked);
  CHECK(checked.error == Lib::ReadError::NoSuchRecord);
}

TEST_CASE("A Reader tells a record without a $STANDARD_INFORMATION",
          "[search][reader]") {
  SECTION("none at all") {
    const TempImage image(TestFake::BuildFakeNtfsImageWithSmallResidentData());
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());

    const auto info = reader.GetStandardInformation(HandleOf(5));

    CHECK_FALSE(info);
    CHECK(info.error == Lib::ReadError::NoSuchAttribute);
  }

  SECTION("one shorter than the NTFS 1 form") {
    std::vector<BYTE> bytes =
        TestFake::BuildFakeNtfsImageWithLegacyStandardInformation();
    const ULONGLONG record = TestSearch::RecordOffsetIn(
        bytes, TestFake::legacy_standard_information_record_idx);
    // The first attribute starts at the offset the header gives, and its value
    // length sits 16 bytes into it.
    WORD first_attribute = 0;
    std::memcpy(&first_attribute, &bytes.at(record + 0x14),
                sizeof(first_attribute));
    const DWORD value_length = 20;
    std::memcpy(&bytes.at(record + first_attribute + 0x10), &value_length,
                sizeof(value_length));
    const TempImage image(bytes);
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());

    const auto info = reader.GetStandardInformation(
        HandleOf(TestFake::legacy_standard_information_record_idx));

    CHECK_FALSE(info);
    CHECK(info.error == Lib::ReadError::RecordDamaged);
  }

  SECTION("one of the NTFS 1 form") {
    const TempImage image(
        TestFake::BuildFakeNtfsImageWithLegacyStandardInformation());
    Lib::Reader reader({.target = image.Target()});
    REQUIRE(reader.IsOpen());

    const auto info = reader.GetStandardInformation(
        HandleOf(TestFake::legacy_standard_information_record_idx));

    REQUIRE(info);
    CHECK_FALSE(info.value.has_extended);
  }
}

TEST_CASE("A Reader tells the name of a record that has none",
          "[search][reader]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithSmallResidentData());
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  SECTION("a record with no $FILE_NAME") {
    const auto name = reader.GetFileName(HandleOf(5));

    CHECK_FALSE(name);
    CHECK(name.error == Lib::ReadError::NoSuchAttribute);
  }

  SECTION("a record that does not exist") {
    const auto name = reader.GetFileName(HandleOf(1000000));

    CHECK_FALSE(name);
    CHECK(name.error == Lib::ReadError::NoSuchRecord);
  }
}

TEST_CASE("A Reader gives the name a handle points at", "[search][reader]") {
  std::vector<TestFake::FakeTreeNode> nodes;
  nodes.push_back(
      {.links = {{.name = L"ALIAS~1.TXT", .name_space = 2}}, .data_size = 4});
  nodes.push_back({.links = {{.name = L"plain.txt"}}, .data_size = 4});
  const TempImage image(TestFake::BuildFakeNtfsImageWithNameTree(nodes));
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  SECTION("a name index past the names") {
    Lib::Handle handle = HandleOf(TestSearch::Record(1));
    handle.name_index = 7;

    const auto name = reader.GetFileName(handle);

    CHECK_FALSE(name);
    CHECK(name.error == Lib::ReadError::NoSuchAttribute);
  }

  SECTION("a record whose only name is a DOS alias, in another folder") {
    Lib::Handle handle = HandleOf(TestSearch::Record(0));
    handle.parent_record = 999;

    const auto name = reader.GetFileName(handle);

    REQUIRE(name);
    CHECK(name.value.name == u"ALIAS~1.TXT");
  }
}
