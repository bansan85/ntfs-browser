#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "external-mft-support.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"

namespace {

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::OpenStandaloneMftVolume;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RecoveringOptions;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::StrictOptions;

// $Boot, the file that sits at the very start of the volume.
constexpr ULONGLONG boot_record = 7;

// The record of ntfs3_attrlist.raw whose $ATTRIBUTE_LIST names the others, and
// the extension records it spreads its $FILE_NAME over.
constexpr ULONGLONG attribute_list_owner = 27;
constexpr ULONGLONG first_extension = 28;
constexpr ULONGLONG last_extension = 38;

std::filesystem::path Fixture(std::string_view name) {
  return RequireCorpusFile(DfirNtfsDir() / "test_data" /
                           std::u8string(name.begin(), name.end()));
}

// The $MFT of the museum's damaged volume, 628 records of 1024 bytes.
std::filesystem::path Xw() {
  return RequireCorpusFile(DfirMuseumDir() /
                           "Windows/NTFSArtifacts/MFT/Unknown/xw/$MFT");
}

// What a parse of one record gives.
struct Outcome {
  bool parsed{false};
  bool attrs_ok{false};
  std::size_t file_names{0};
  std::size_t data_streams{0};
};

Outcome ParseRecord(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                    ULONGLONG record_number) {
  FileRecord<Cache::Strategy::NoCache> record(volume);
  Outcome outcome;
  outcome.parsed = record.ParseFileRecord(record_number);
  outcome.attrs_ok = outcome.parsed && record.ParseAttrs();
  outcome.file_names = record.GetAttr(Attr::Type::FileName).size();
  outcome.data_streams = record.GetAttr(Attr::Type::Data).size();
  return outcome;
}

// The $MFT of xw under the options. Its record 0 needs the repair of the
// declared extent.
std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenXw(const VolumeOptions& options) {
  return OpenStandaloneMftVolume(Xw(), options, true);
}

}  // namespace

TEST_CASE("The $Boot file of a real $MFT starts at cluster 0",
          "[dfir-corpora][integration][malformed-record]") {
  const char* name =
      GENERATE("mapping_pairs_1.mft", "4k-large.mft", "orphan.mft");
  CAPTURE(name);
  const auto volume = OpenStandaloneMftVolume(Fixture(name));

  FileRecord<Cache::Strategy::NoCache> record(*volume);
  record.SetAttrMask(Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(boot_record));
  REQUIRE(record.ParseAttrs());
  const auto& streams = record.GetAttr(Attr::Type::Data);
  REQUIRE(streams.size() == 1);
  const NTFS_BROWSER_NAMESPACE::Alloc::StreamExtents found =
      streams.front()->GetExtents();
  CHECK(found.complete);
  // Two clusters at LCN 0, the first of them with an offset field of zero.
  REQUIRE(found.extents.size() == 1);
  CHECK(found.extents.front().lcn == 0);
  CHECK(found.extents.front().clusters == 2);
}

TEST_CASE("The extension records of a real volume use the NTFS 3.0 header",
          "[dfir-corpora][integration][malformed-record]") {
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<MemoryDiskReader>(
          ReadCorpusFile(Fixture("ntfs3_attrlist.raw"))));
  REQUIRE(volume.IsVolumeOK());

  // Records 28 to 38 hold the 100 $FILE_NAME of record 27, whose update
  // sequence array begins at 0x2A.
  for (ULONGLONG number = first_extension; number <= last_extension; number++) {
    CAPTURE(number);
    FileRecord<Cache::Strategy::NoCache> record(volume);
    REQUIRE(record.ParseFileRecord(number));
    CHECK(record.IsExtensionRecord());
    CHECK(record.GetBaseRecordReference() == attribute_list_owner);
    CHECK(record.ParseAttrs());
  }

  const Outcome owner = ParseRecord(volume, attribute_list_owner);
  CHECK(owner.attrs_ok);
  CHECK(owner.file_names == 100);
  CHECK(owner.data_streams == 1);
}

TEST_CASE("Freed records of a damaged $MFT are read past their used size",
          "[dfir-corpora][integration][malformed-record]") {
  // The end marker of these records lies past the used size their header
  // declares, and their fixups verify.
  const ULONGLONG record_number = GENERATE(496ULL, 524ULL);
  CAPTURE(record_number);
  for (const VolumeOptions& options : {StrictOptions(), RecoveringOptions()}) {
    CAPTURE(options.recover_errors);
    const auto volume = OpenXw(options);
    const Outcome outcome = ParseRecord(*volume, record_number);
    CHECK(outcome.parsed);
    CHECK(outcome.attrs_ok);
    CHECK(outcome.file_names == 1);
    CHECK(outcome.data_streams == 1);
  }
}

TEST_CASE(
    "A freed record whose run overruns its attribute is rejected, or kept "
    "when recovering",
    "[dfir-corpora][integration][malformed-record]") {
  // Record 180 maps 8 clusters in a $DATA that declares 4.
  const Outcome strict = ParseRecord(*OpenXw(StrictOptions()), 180);
  CHECK(strict.parsed);
  CHECK_FALSE(strict.attrs_ok);
  CHECK(strict.file_names == 0);
  CHECK(strict.data_streams == 0);

  const Outcome recovering = ParseRecord(*OpenXw(RecoveringOptions()), 180);
  CHECK(recovering.attrs_ok);
  CHECK(recovering.file_names == 2);
  CHECK(recovering.data_streams == 1);
}

TEST_CASE(
    "A record with garbage where its end marker should be rejects, or keeps "
    "the attributes before the garbage",
    "[dfir-corpora][integration][malformed-record]") {
  const ULONGLONG record_number = GENERATE(202ULL, 442ULL, 612ULL, 613ULL);
  CAPTURE(record_number);

  const Outcome strict = ParseRecord(*OpenXw(StrictOptions()), record_number);
  CHECK(strict.parsed);
  CHECK_FALSE(strict.attrs_ok);
  CHECK(strict.file_names == 0);
  CHECK(strict.data_streams == 0);

  const Outcome recovering =
      ParseRecord(*OpenXw(RecoveringOptions()), record_number);
  CHECK(recovering.parsed);
  CHECK_FALSE(recovering.attrs_ok);
  CHECK(recovering.file_names >= 1);
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
  // The $DATA of record 202 is compressed, which is rejected.
  if (record_number == 202) {
    CHECK(recovering.data_streams == 0);
    return;
  }
#endif
  CHECK(recovering.data_streams == 1);
}
