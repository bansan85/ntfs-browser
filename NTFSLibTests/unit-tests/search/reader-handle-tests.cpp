#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/types.h>

#include "reader-support.h"
#include "scan-support.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::RowNamed;
using TestSearch::TempImage;

namespace {

// Offset of the volume serial number in the boot sector.
constexpr std::size_t serial_offset = 0x48;

// Bytes in the volume serial number.
constexpr std::size_t serial_size = 8;

// The tree image with the bytes of the volume serial number set from seed.
std::vector<BYTE> TreeImageWithSerial(BYTE seed) {
  std::vector<BYTE> image = TestSearch::TreeImage();
  for (std::size_t i = 0; i < serial_size; ++i) {
    image.at(serial_offset + i) = static_cast<BYTE>(seed + i);
  }
  return image;
}

}  // namespace

TEST_CASE("A handle from a scan names its file again in a Reader",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  REQUIRE(reader.IsOpen());

  SECTION("a live file") {
    const auto row = RowNamed(rows, u"a.txt");
    REQUIRE(row.name == u"a.txt");

    const auto checked = reader.Check(row.handle);

    REQUIRE(checked);
    CHECK(checked.value == Lib::Standing::Live);
  }

  SECTION("a deleted file") {
    const auto row = RowNamed(rows, u"gone.txt");
    REQUIRE(row.name == u"gone.txt");
    CHECK(row.standing == Lib::Standing::Deleted);

    const auto checked = reader.Check(row.handle);

    REQUIRE(checked);
    CHECK(checked.value == Lib::Standing::Deleted);
  }

  SECTION("every entry of the volume") {
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
      INFO(row.record);
      CHECK(reader.Check(row.handle));
    }
  }
}

TEST_CASE("A handle of a directory engine names its file too",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::DirectoryFullCache;
  options.recursive = true;
  const auto outcome = TestSearch::RunScan(options);
  const auto row = RowNamed(outcome.rows, u"c.txt");
  REQUIRE(row.name == u"c.txt");
  CHECK(row.handle.name_index == Lib::Handle::unknown_name);
  Lib::Reader reader({.target = image.Target()});

  const auto name = reader.GetFileName(row.handle);

  REQUIRE(name);
  CHECK(name.value.name == u"c.txt");
  CHECK(name.value.parent_record == row.parent_record);
}

TEST_CASE("A handle keeps working after the entry and the scan are gone",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Handle kept;
  {
    const auto rows = TestSearch::ListImage(image);
    kept = RowNamed(rows, u"root.txt").handle;
  }
  Lib::Reader reader({.target = image.Target()});

  const auto name = reader.GetFileName(kept);

  REQUIRE(name);
  CHECK(name.value.name == u"root.txt");
}

TEST_CASE("The handle of the MftList engine says which name it came from",
          "[search][reader]") {
  const TempImage image(TestSearch::HardLinkImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto one = RowNamed(rows, u"one.txt");
  const auto two = RowNamed(rows, u"two.txt");
  REQUIRE(one.record == two.record);
  CHECK(one.handle.name_index != two.handle.name_index);

  const auto first = reader.GetFileName(one.handle);
  const auto second = reader.GetFileName(two.handle);

  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value.name == u"one.txt");
  CHECK(second.value.name == u"two.txt");
  CHECK(first.value.index == one.handle.name_index);
}

TEST_CASE("A Reader refuses a handle whose record changed sequence",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  Lib::Handle handle = RowNamed(rows, u"a.txt").handle;
  REQUIRE(handle.sequence != 0);

  SECTION("another sequence") {
    ++handle.sequence;

    const auto checked = reader.Check(handle);

    CHECK(checked.error == Lib::ReadError::SequenceChanged);
    std::vector<std::uint8_t> bytes(4, 0xEE);
    const auto read = reader.Read(handle, u"", 0, bytes);
    CHECK(read.error == Lib::ReadError::SequenceChanged);
    CHECK(bytes == std::vector<std::uint8_t>(4, 0xEE));
    CHECK(reader.GetFileNames(handle).error == Lib::ReadError::SequenceChanged);
    CHECK(reader.GetStandardInformation(handle).error ==
          Lib::ReadError::SequenceChanged);
  }

  SECTION("sequence 0 takes any") {
    handle.sequence = 0;

    CHECK(reader.Check(handle));
  }

  SECTION("a handle that saw the file deleted, on a record in use") {
    handle.deleted = true;

    CHECK(reader.Check(handle).error == Lib::ReadError::SequenceChanged);
  }
}

TEST_CASE("A Reader takes a deleted file under its own sequence or the next",
          "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  Lib::Handle handle = RowNamed(rows, u"gone.txt").handle;

  SECTION("the sequence the entry had") { CHECK(reader.Check(handle)); }

  SECTION("one before: the file was deleted after the entry") {
    handle.deleted = false;
    --handle.sequence;

    CHECK(reader.Check(handle));
  }

  SECTION("another sequence is another file") {
    handle.deleted = false;
    handle.sequence = static_cast<std::uint16_t>(handle.sequence + 5);

    CHECK(reader.Check(handle).error == Lib::ReadError::SequenceChanged);
  }
}

TEST_CASE("A Reader refuses a handle of another volume", "[search][reader]") {
  const TempImage first(TreeImageWithSerial(1));
  const TempImage second(TreeImageWithSerial(2));
  const auto rows = TestSearch::ListImage(first);
  const Lib::Handle handle = RowNamed(rows, u"a.txt").handle;
  REQUIRE(handle.volume != 0);
  Lib::Reader own({.target = first.Target()});
  Lib::Reader other({.target = second.Target()});

  CHECK(own.Check(handle));
  CHECK(other.Check(handle).error == Lib::ReadError::WrongVolume);
  std::vector<std::uint8_t> bytes(4);
  CHECK(other.Read(handle, u"", 0, bytes).error == Lib::ReadError::WrongVolume);
}

TEST_CASE("A Reader refuses a record that is not there", "[search][reader]") {
  const TempImage image(TestSearch::TreeImage());
  Lib::Reader reader({.target = image.Target()});

  SECTION("past the end of the $MFT") {
    const auto checked = reader.Check({.record = 1ULL << 40});

    CHECK(checked.error == Lib::ReadError::NoSuchRecord);
  }

  SECTION("a slot that holds no record") {
    const auto checked = reader.Check({.record = 100});

    CHECK_FALSE(checked);
  }
}

TEST_CASE("A Reader of a target that does not open fails every call cleanly",
          "[search][reader]") {
  (void)TestSupport::TakeCapturedLog();
  Lib::Reader reader({.target = u"/this/path/does/not/exist.img"});

  CHECK_FALSE(reader.IsOpen());
  CHECK(reader.Check({.record = 5}).error == Lib::ReadError::NotOpen);
  CHECK(reader.GetFileNames({.record = 5}).error == Lib::ReadError::NotOpen);
  CHECK(reader.GetStreams({.record = 5}).error == Lib::ReadError::NotOpen);
  std::vector<std::uint8_t> bytes(4);
  CHECK(reader.Read({.record = 5}, u"", 0, bytes).error ==
        Lib::ReadError::NotOpen);
  CHECK_FALSE(TestSupport::TakeCapturedLog().empty());
}

TEST_CASE("A Reader of a file that is no NTFS volume does not open",
          "[search][reader]") {
  const TempImage image(std::vector<BYTE>(65536, 'x'));
  (void)TestSupport::TakeCapturedLog();

  Lib::Reader reader({.target = image.Target()});

  CHECK_FALSE(reader.IsOpen());
  CHECK(reader.Check({.record = 5}).error == Lib::ReadError::NotOpen);
}

TEST_CASE("The entry of a scan builds the handle from its own fields",
          "[search][reader]") {
  Lib::Entry entry;
  entry.record = 77;
  entry.sequence = 3;
  entry.parent_record = 16;
  entry.name_index = 2;
  entry.volume = 0xABCD;
  entry.standing = Lib::Standing::Reused;

  const Lib::Handle handle = entry.GetHandle();

  CHECK(handle.record == 77);
  CHECK(handle.sequence == 3);
  CHECK(handle.parent_record == 16);
  CHECK(handle.name_index == 2);
  CHECK(handle.volume == 0xABCD);
  CHECK(handle.deleted);
}
