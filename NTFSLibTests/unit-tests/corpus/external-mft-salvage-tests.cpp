#include <ntfs-browser/win-types.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "external-mft-support.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace {

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::OpenStandaloneMftVolume;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RecoveringOptions;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::ScanStandaloneMft;
using NTFS_BROWSER_NAMESPACE::Tests::StandaloneMftScan;
using NTFS_BROWSER_NAMESPACE::Tests::StrictOptions;
using NTFS_BROWSER_NAMESPACE::Tests::TempFile;

// Bytes of a record in the $MFT files used here.
constexpr std::size_t record_size = 1024;

// Offset, in a record, of the last word of its first and of its second
// sector: the words the update sequence array protects.
constexpr std::size_t first_sector_end = 510;
constexpr std::size_t second_sector_end = 1022;

// A word that is neither the update sequence number of a record of the files
// used here nor a value its array saved.
constexpr std::uint16_t foreign_word = 0x7777;

// The freed file of xw whose first sector fails its check: a $DATA of 34 304
// bytes. Its one run (17 clusters at LCN 3614) is longer than the attribute
// declares, so a recovering parse keeps the size and drops the run.
constexpr ULONGLONG xw_freed_file = 179;

// The records of xw whose update sequence check fails on one sector and whose
// signature is FILE.
constexpr std::array<ULONGLONG, 10> xw_failed_fixups = {
    179, 540, 546, 551, 554, 566, 567, 568, 586, 616};

// The records of xw whose signature is BAAD.
constexpr std::array<ULONGLONG, 2> xw_baad = {500, 565};

std::filesystem::path Xw() {
  return RequireCorpusFile(DfirMuseumDir() /
                           "Windows/NTFSArtifacts/MFT/Unknown/xw/$MFT");
}

std::filesystem::path Orphan() {
  return RequireCorpusFile(DfirNtfsDir() / "test_data" / "orphan.mft");
}

// Writes value at offset of bytes.
void PutWord(std::vector<BYTE>& bytes, std::size_t offset,
             std::uint16_t value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(value));
}

// orphan.mft with a foreign word at each given offset of record `record`.
std::vector<BYTE>
    OrphanWithForeignWords(ULONGLONG record,
                           const std::vector<std::size_t>& offsets) {
  std::vector<BYTE> bytes = ReadCorpusFile(Orphan());
  for (const std::size_t offset : offsets) {
    PutWord(bytes, (record * record_size) + offset, foreign_word);
  }
  return bytes;
}

// Freed file record of orphan.mft that the tests damage: 2.txt.
constexpr ULONGLONG orphan_freed_file = 44;

}  // namespace

TEST_CASE("A freed record of xw with one failed sector is salvaged",
          "[dfir-corpora][integration][mft][salvage]") {
  const auto strict = OpenStandaloneMftVolume(Xw(), StrictOptions(), true);
  FileRecord<Cache::Strategy::NoCache> strict_record(*strict);
  CHECK_FALSE(strict_record.ParseFileRecord(xw_freed_file));

  const auto recovering =
      OpenStandaloneMftVolume(Xw(), RecoveringOptions(), true);
  FileRecord<Cache::Strategy::NoCache> record(*recovering);
  REQUIRE(record.ParseFileRecord(xw_freed_file));
  CHECK(record.IsDeleted());
  REQUIRE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::FileName).size() >= 1);
  const auto& streams = record.GetAttr(Attr::Type::Data);
  REQUIRE(streams.size() == 1);
  CHECK(streams.front()->GetDataSize() == 34304);
}

TEST_CASE("A scan of xw keeps the records with a failed sector when recovering",
          "[dfir-corpora][integration][mft][salvage]") {
  const StandaloneMftScan strict =
      ScanStandaloneMft(Xw(), Cache::Strategy::NoCache, StrictOptions(), true);
  const StandaloneMftScan recovering = ScanStandaloneMft(
      Xw(), Cache::Strategy::NoCache, RecoveringOptions(), true);

  for (const ULONGLONG record : xw_failed_fixups) {
    CAPTURE(record);
    CHECK(strict.tree.Find(record) == nullptr);
    CHECK(recovering.tree.Find(record) != nullptr);
  }
  // A BAAD record is never salvaged.
  for (const ULONGLONG record : xw_baad) {
    CAPTURE(record);
    CHECK(strict.tree.Find(record) == nullptr);
    CHECK(recovering.tree.Find(record) == nullptr);
  }

  // The ten records move from the unreadable slots to the kept ones.
  CHECK(strict.tree.Stats().unreadable == 12);
  CHECK(recovering.tree.Stats().unreadable == 2);
  CHECK(recovering.tree.Stats().in_use + recovering.tree.Stats().deleted ==
        strict.tree.Stats().in_use + strict.tree.Stats().deleted +
            xw_failed_fixups.size());
}

TEST_CASE("A record with one failed sector is salvaged only when recovering",
          "[dfir-corpora][integration][mft][salvage]") {
  const std::size_t sector_end = GENERATE(first_sector_end, second_sector_end);
  CAPTURE(sector_end);
  const TempFile file(OrphanWithForeignWords(orphan_freed_file, {sector_end}));

  const StandaloneMftScan strict =
      ScanStandaloneMft(file.Path(), Cache::Strategy::NoCache, StrictOptions());
  CHECK(strict.tree.Find(orphan_freed_file) == nullptr);

  for (const Cache::Strategy strategy :
       {Cache::Strategy::NoCache, Cache::Strategy::FullCache}) {
    const StandaloneMftScan recovering =
        ScanStandaloneMft(file.Path(), strategy, RecoveringOptions());
    const Mft::MftTree::Entry* entry = recovering.tree.Find(orphan_freed_file);
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->in_use);
    REQUIRE(entry->names.size() == 1);
    CHECK(entry->names.front().name == L"2.txt");
  }
}

TEST_CASE("A record whose every sector fails is never salvaged",
          "[dfir-corpora][integration][mft][salvage]") {
  const TempFile file(OrphanWithForeignWords(
      orphan_freed_file, {first_sector_end, second_sector_end}));
  for (const VolumeOptions& options : {StrictOptions(), RecoveringOptions()}) {
    CAPTURE(options.recover_errors);
    const StandaloneMftScan scan =
        ScanStandaloneMft(file.Path(), Cache::Strategy::NoCache, options);
    CHECK(scan.tree.Find(orphan_freed_file) == nullptr);
  }
}

TEST_CASE("A BAAD record is never salvaged",
          "[dfir-corpora][integration][mft][salvage]") {
  std::vector<BYTE> bytes = ReadCorpusFile(Orphan());
  std::memcpy(&bytes.at(orphan_freed_file * record_size), "BAAD", 4);
  const TempFile file(bytes);
  for (const VolumeOptions& options : {StrictOptions(), RecoveringOptions()}) {
    CAPTURE(options.recover_errors);
    const StandaloneMftScan scan =
        ScanStandaloneMft(file.Path(), Cache::Strategy::NoCache, options);
    CHECK(scan.tree.Find(orphan_freed_file) == nullptr);
  }
}
