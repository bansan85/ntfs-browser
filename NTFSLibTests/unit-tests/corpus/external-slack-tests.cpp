#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "external-mft-support.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Record = NTFS_BROWSER_NAMESPACE::Record;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::EntryUtcTicks;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RecoveringOptions;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::ScanStandaloneMft;
using NTFS_BROWSER_NAMESPACE::Tests::StrictOptions;
using NTFS_BROWSER_NAMESPACE::Tests::UtcTicks;

// The directory record of the three slack fixtures, whose resident $INDEX_ROOT
// shrank and left its old entries after the end marker.
constexpr ULONGLONG slack_directory = 39;

// $Extend, whose record keeps the name "$RmMetadata" of an old index entry in
// every fixture.
constexpr ULONGLONG extend_record = 11;

std::filesystem::path Fixture(std::string_view name) {
  return RequireCorpusFile(DfirNtfsDir() / "test_data" /
                           std::u8string(name.begin(), name.end()));
}

// A time in whole microseconds, the precision the documentation keeps. Entry
// times are local on Windows: EntryUtcTicks() undoes it.
std::uint64_t Micros(std::uint64_t ticks) {
  constexpr std::uint64_t ticks_per_microsecond = 10;
  return ticks / ticks_per_microsecond;
}

// What a carved name says.
struct Expected {
  std::wstring name;
  bool partial;
  bool parent_known;
  // Creation time in UTC microseconds; 0 when the times are unknown.
  std::uint64_t create_micros;
};

Expected Complete(std::wstring name, std::uint64_t create_ticks) {
  return {std::move(name), false, true, Micros(create_ticks)};
}

std::vector<Expected> Describe(const Mft::MftTree::Entry& entry) {
  std::vector<Expected> described;
  for (const Record::CarvedName& name : entry.carved_names) {
    described.push_back(
        {name.name, name.partial, name.parent_known,
         name.times_known ? Micros(EntryUtcTicks(name.create_time)) : 0});
  }
  return described;
}

// Checks the names of one entry against the expected list, in order.
void CheckNames(const Mft::MftTree::Entry& entry,
                const std::vector<Expected>& expected) {
  const std::vector<Expected> carved = Describe(entry);
  REQUIRE(carved.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    INFO("record " << entry.record << " name " << i);
    CHECK(carved.at(i).name == expected.at(i).name);
    CHECK(carved.at(i).partial == expected.at(i).partial);
    CHECK(carved.at(i).parent_known == expected.at(i).parent_known);
    CHECK(carved.at(i).create_micros == expected.at(i).create_micros);
  }
}

// The records of a scan that carry carved names.
std::set<ULONGLONG> RecordsWithCarvedNames(const Mft::MftTree& tree) {
  std::set<ULONGLONG> records;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (!entry.carved_names.empty()) {
      records.insert(entry.record);
    }
  }
  return records;
}

// Whether two lists of carved names say the same thing, field by field.
bool SameNames(const std::vector<Record::CarvedName>& left,
               const std::vector<Record::CarvedName>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    const Record::CarvedName& a = left.at(i);
    const Record::CarvedName& b = right.at(i);
    if (a.name != b.name || a.parent_record != b.parent_record ||
        a.parent_sequence != b.parent_sequence ||
        a.parent_known != b.parent_known || a.times_known != b.times_known ||
        a.partial != b.partial || a.name_space != b.name_space ||
        EntryUtcTicks(a.create_time) != EntryUtcTicks(b.create_time) ||
        EntryUtcTicks(a.write_time) != EntryUtcTicks(b.write_time) ||
        EntryUtcTicks(a.change_time) != EntryUtcTicks(b.change_time) ||
        EntryUtcTicks(a.access_time) != EntryUtcTicks(b.access_time)) {
      return false;
    }
  }
  return true;
}

// $RmMetadata is the one leftover name of $Extend. Its parent reference sits
// under the end marker, its times survive.
void CheckRmMetadata(const Mft::MftTree& tree) {
  const Mft::MftTree::Entry* extend = tree.Find(extend_record);
  REQUIRE(extend != nullptr);
  REQUIRE(extend->carved_names.size() == 1);
  const Record::CarvedName& name = extend->carved_names.front();
  CHECK(name.name == L"$RmMetadata");
  CHECK(name.partial);
  CHECK_FALSE(name.parent_known);
  CHECK(name.times_known);
}

template <Cache::Strategy S>
void CheckSlackFixture(std::string_view file, std::string_view live_name,
                       const std::vector<Expected>& expected) {
  const auto scan = ScanStandaloneMft(Fixture(file), S, RecoveringOptions());

  const Mft::MftTree::Entry* directory = scan.tree.Find(slack_directory);
  REQUIRE(directory != nullptr);
  REQUIRE_FALSE(directory->names.empty());
  CHECK(directory->names.front().name ==
        std::wstring(live_name.begin(), live_name.end()));

  CheckNames(*directory, expected);
  for (const Record::CarvedName& name : directory->carved_names) {
    if (name.parent_known) {
      CHECK(name.parent_record == slack_directory);
      CHECK(name.parent_sequence == 1);
    }
  }

  // $Extend keeps an old name too: nothing else does.
  CheckRmMetadata(scan.tree);
  CHECK(RecordsWithCarvedNames(scan.tree) ==
        std::set<ULONGLONG>{extend_record, slack_directory});
  CHECK(scan.tree.Stats().carved_names == expected.size() + 1);
}

// Reads a little-endian T at offset of bytes.
template <typename T>
T Read(const std::vector<BYTE>& bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, &bytes.at(offset), sizeof(value));
  return value;
}

// The numbers of the records whose bytes after the end marker are all zero,
// found by walking the attribute headers of the raw file: the library is not
// asked. The two bytes that end each 512-byte sector hold the update sequence
// word and are not looked at. Only records that reach their end marker count.
std::set<ULONGLONG> RecordsWithEmptySlack(const std::vector<BYTE>& bytes,
                                          std::size_t record_size) {
  constexpr std::size_t sector = 512;
  constexpr std::size_t offset_of_attr = 0x14;
  constexpr DWORD file_magic = 0x454C4946;
  constexpr DWORD end_marker = 0xFFFFFFFF;
  constexpr std::size_t terminator = 8;
  std::set<ULONGLONG> empty;
  for (std::size_t start = 0; start + record_size <= bytes.size();
       start += record_size) {
    if (Read<DWORD>(bytes, start) != file_magic) {
      continue;
    }
    std::size_t offset = Read<WORD>(bytes, start + offset_of_attr);
    while (offset + 8 <= record_size &&
           Read<DWORD>(bytes, start + offset) != end_marker) {
      const DWORD length = Read<DWORD>(bytes, start + offset + 4);
      if (length < 8) {
        offset = record_size;
        break;
      }
      offset += length;
    }
    if (offset + terminator > record_size) {
      continue;
    }
    bool zeros = true;
    for (std::size_t i = offset + terminator; i < record_size && zeros; ++i) {
      zeros = (i % sector) >= sector - 2 || bytes.at(start + i) == 0;
    }
    if (zeros) {
      empty.insert(start / record_size);
    }
  }
  return empty;
}

// A creation time the documentation of dfir_ntfs gives, in UTC.
std::uint64_t Created(int year, unsigned month, unsigned day, unsigned hour,
                      unsigned minute, unsigned second, unsigned microsecond) {
  return UtcTicks(year, month, day, hour, minute, second, microsecond);
}

}  // namespace

TEST_CASE("slack.mft: record 39 gives its three old directory entries",
          "[corpus][slack][external-mft]") {
  const std::vector<Expected> expected = {
      Complete(L"New Text Document - Copy (3).txt",
               Created(2019, 2, 18, 14, 11, 58, 61887)),
      Complete(L"New Text Document - Copy.txt",
               Created(2019, 2, 18, 14, 11, 58, 46259)),
      Complete(L"New Text Document.txt",
               Created(2019, 2, 18, 14, 11, 54, 984000))};
  CheckSlackFixture<Cache::Strategy::NoCache>("slack.mft", "test_folder",
                                              expected);
  CheckSlackFixture<Cache::Strategy::FullCache>("slack.mft", "test_folder",
                                                expected);
}

TEST_CASE("slack-2.mft: two old names, one of them without its times",
          "[corpus][slack][external-mft]") {
  // The name that sits first on disk lost its parent and its times to the live
  // attributes. The two identical copies of t3.txt are one candidate.
  const std::vector<Expected> expected = {
      {L"t2.txt", true, false, 0},
      Complete(L"t3.txt", Created(2020, 9, 17, 15, 34, 29, 193751))};
  CheckSlackFixture<Cache::Strategy::NoCache>("slack-2.mft", "test", expected);
  CheckSlackFixture<Cache::Strategy::FullCache>("slack-2.mft", "test",
                                                expected);
}

TEST_CASE("slack-3.mft: a name whose parent reference was overwritten",
          "[corpus][slack][external-mft]") {
  const std::vector<Expected> expected = {
      {L"t3.txt", true, false,
       Micros(Created(2020, 9, 17, 15, 34, 29, 193751))}};
  CheckSlackFixture<Cache::Strategy::NoCache>("slack-3.mft", "test", expected);
  CheckSlackFixture<Cache::Strategy::FullCache>("slack-3.mft", "test",
                                                expected);
}

TEST_CASE("slack fixtures: strict mode and deleted-only mode carve nothing",
          "[corpus][slack][external-mft]") {
  for (const std::string_view file :
       {"slack.mft", "slack-2.mft", "slack-3.mft"}) {
    for (const VolumeOptions& options :
         {StrictOptions(true), StrictOptions(false),
          VolumeOptions{.include_deleted = false, .recover_errors = true}}) {
      for (const Cache::Strategy strategy :
           {Cache::Strategy::NoCache, Cache::Strategy::FullCache}) {
        const auto scan = ScanStandaloneMft(Fixture(file), strategy, options);

        INFO(file);
        CHECK(RecordsWithCarvedNames(scan.tree).empty());
        CHECK(scan.tree.Stats().carved_names == 0);
      }
    }
  }
}

TEST_CASE("slack fixtures: both cache strategies carve the same names",
          "[corpus][slack][external-mft]") {
  for (const std::string_view file :
       {"slack.mft", "slack-2.mft", "slack-3.mft", "mapping_pairs_1.mft"}) {
    const auto no_cache = ScanStandaloneMft(
        Fixture(file), Cache::Strategy::NoCache, RecoveringOptions());
    const auto full_cache = ScanStandaloneMft(
        Fixture(file), Cache::Strategy::FullCache, RecoveringOptions());

    REQUIRE(no_cache.tree.Entries().size() == full_cache.tree.Entries().size());
    for (std::size_t i = 0; i < no_cache.tree.Entries().size(); ++i) {
      INFO(file << " entry " << i);
      CHECK(SameNames(no_cache.tree.Entries().at(i).carved_names,
                      full_cache.tree.Entries().at(i).carved_names));
    }
  }
}

TEST_CASE("fixtures without a stale directory entry carve only $RmMetadata",
          "[corpus][slack][external-mft]") {
  for (const std::string_view file :
       {"4k-large.mft", "unicode.mft", "orphan.mft", "deleted.mft"}) {
    for (const Cache::Strategy strategy :
         {Cache::Strategy::NoCache, Cache::Strategy::FullCache}) {
      const auto scan =
          ScanStandaloneMft(Fixture(file), strategy, RecoveringOptions());

      INFO(file);
      CheckRmMetadata(scan.tree);
      CHECK(RecordsWithCarvedNames(scan.tree) ==
            std::set<ULONGLONG>{extend_record});
    }
  }
}

TEST_CASE("mapping_pairs_1.mft: the old entries of a directory of sparse files",
          "[corpus][slack][external-mft]") {
  const auto scan =
      ScanStandaloneMft(Fixture("mapping_pairs_1.mft"),
                        Cache::Strategy::NoCache, RecoveringOptions());

  CHECK(RecordsWithCarvedNames(scan.tree) ==
        std::set<ULONGLONG>{extend_record, 37});
  const Mft::MftTree::Entry* directory = scan.tree.Find(37);
  REQUIRE(directory != nullptr);
  std::set<std::wstring> names;
  for (const Record::CarvedName& name : directory->carved_names) {
    names.insert(name.name);
  }
  CHECK(names == std::set<std::wstring>{L"entirely_sparse", L"fragmented.txt",
                                        L"partially_sparse",
                                        L"partially_sparse_2"});
}

TEST_CASE("records with an empty slack carve no name",
          "[corpus][slack][external-mft]") {
  struct Case {
    std::string_view file;
    std::size_t record_size;
  };

  for (const Case& test_case :
       {Case{"slack.mft", 1024}, Case{"unicode.mft", 1024},
        Case{"4k-large.mft", 4096}, Case{"orphan.mft", 1024},
        Case{"deleted.mft", 1024}}) {
    const std::filesystem::path file = Fixture(test_case.file);
    const std::set<ULONGLONG> empty =
        RecordsWithEmptySlack(ReadCorpusFile(file), test_case.record_size);
    // The check would prove nothing without records to look at.
    REQUIRE(empty.size() >= 10);

    const auto scan =
        ScanStandaloneMft(file, Cache::Strategy::NoCache, RecoveringOptions());

    INFO(test_case.file);
    for (const Mft::MftTree::Entry& entry : scan.tree.Entries()) {
      if (empty.contains(entry.record)) {
        CHECK(entry.carved_names.empty());
      }
    }
  }
}
