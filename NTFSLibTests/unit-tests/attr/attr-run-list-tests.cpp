#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/base.h>  // IWYU pragma: keep
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "data/header-non-resident.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::FakeRunHost;

namespace {

// Encoded run of the second $MFT extent: a 4-byte LCN delta and a 1-byte
// length, then the terminator. The delta puts it at LCN 41.
constexpr std::array<BYTE, 6> second_extent_run{0x41, 0x01, 0x29, 0, 0, 0};

// Real size, in bytes, that the base $MFT DATA attribute claims. Above the
// mapped range, so the mapped range alone limits GetRecordsCount().
constexpr ULONGLONG claimed_mft_size = 64ULL * 1024ULL;

// Byte offset of the record holding $MFT's base file record.
size_t MftBaseRecordOffset(const std::vector<BYTE>& image) {
  NTFS_BROWSER_NAMESPACE::Data::NtfsBpb bpb{};
  std::memcpy(&bpb, image.data(), sizeof(bpb));
  return static_cast<size_t>(bpb.lcn_mft) *
         NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size;
}

// Rewrites the second extent's run as sparse: one cluster with no LCN.
void MakeSecondExtentSparse(std::vector<BYTE>& image) {
  const auto it =
      std::search(image.begin(), image.end(), second_extent_run.begin(),
                  second_extent_run.end());
  REQUIRE(it != image.end());
  const auto run = static_cast<size_t>(it - image.begin());
  image[run] = 0x01;      // Length only: no offset bytes, so not an LCN run.
  image[run + 2] = 0x00;  // Terminates the run list after the length byte.
}

// Sets the claimed real size of $MFT's base DATA attribute, which follows the
// resident $ATTRIBUTE_LIST at the first attribute slot of the base record.
void ClaimMftSize(std::vector<BYTE>& image, ULONGLONG size) {
  const size_t record = MftBaseRecordOffset(image);
  // HeaderCommon::total_size sits at byte 4 of the list attribute.
  DWORD list_size = 0;
  std::memcpy(&list_size, image.data() + record + 48 + 4, sizeof(list_size));
  const size_t data_attr = record + 48 + list_size;
  const size_t real_size_at =
      data_attr +
      offsetof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident, real_size);
  std::memcpy(image.data() + real_size_at, &size, sizeof(size));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A sparse run in a merged $MFT extent yields no records from that extent",
    "[attr-run-list][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
      BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
  {
    const NtfsVolume<S> volume(
        std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
            image));
    REQUIRE(volume.IsVolumeOK());
    FileRecord<S> record(volume);
    CHECK(record.ParseFileRecord(
        NTFS_BROWSER_NAMESPACE::Tests::mft_two_extents_second_vcn));
  }

  MakeSecondExtentSparse(image);
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(image));
  REQUIRE(volume.IsVolumeOK());
  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog(),
             Catch::Matchers::ContainsSubstring("Sparse Data"));

  FileRecord<S> first(volume);
  CHECK(first.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::mft_two_extents_first_vcn));
  FileRecord<S> second(volume);
  CHECK_FALSE(second.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::mft_two_extents_second_vcn));
}

TEMPLATE_TEST_CASE_SIG(
    "A sparse tail does not count towards the $MFT's mapped records",
    "[attr-run-list][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
      BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
  ClaimMftSize(image, claimed_mft_size);
  {
    const NtfsVolume<S> volume(
        std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
            image));
    REQUIRE(volume.IsVolumeOK());
    // Mapped through VCN 32 (33 clusters of 1 KiB), one record per 1 KiB.
    CHECK(volume.GetRecordsCount() == 33);
  }

  MakeSecondExtentSparse(image);
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(image));
  REQUIRE(volume.IsVolumeOK());
  // The sparse extent now stops at VCN 32, one record short.
  CHECK(volume.GetRecordsCount() == 32);
}

TEMPLATE_TEST_CASE_SIG(
    "A rejected data run logs its decode error in both recovery modes",
    "[attr-run-list][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto overflow = [] {
    return NTFS_BROWSER_NAMESPACE::Tests::
        BuildFakeNtfsImageWithOverflowingLcnSum(FakeRunHost::Data);
  };
  const auto negative = [] {
    return NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNegativeLcn(
        FakeRunHost::Data);
  };
  const auto bad_bound = [] {
    return NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithBadDataRun();
  };

  // Parses the root record's $DATA and returns the messages logged meanwhile.
  const auto parse_log = [](std::vector<BYTE> image, VolumeOptions options) {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
            std::move(image)),
        options);
    REQUIRE(volume.IsVolumeOK());
    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    (void)record.ParseAttrs();
    return NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  };

  SECTION("an LCN overflow") {
    CHECK(
        parse_log(overflow(), {}).find("DataRun decode error: LCN overflows") !=
        std::string::npos);
    CHECK(parse_log(overflow(), {.recover_errors = true})
              .find("DataRun decode error: LCN overflows") !=
          std::string::npos);
  }

  SECTION("an LCN underflow") {
    CHECK_THAT(parse_log(negative(), {}),
               Catch::Matchers::ContainsSubstring("DataRun decode error 2"));
    CHECK_THAT(parse_log(negative(), {.recover_errors = true}),
               Catch::Matchers::ContainsSubstring("DataRun decode error 2"));
  }

  SECTION("a run past the declared VCN bound") {
    CHECK(parse_log(bad_bound(), {})
              .find("DataRun decode error: VCN exceeds bound") !=
          std::string::npos);
    CHECK(parse_log(bad_bound(), {.recover_errors = true})
              .find("DataRun decode error: VCN exceeds bound") !=
          std::string::npos);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A data run list whose LCN goes negative is rejected, or kept up to the "
    "bad run",
    "[attr-run-list][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("strict: the whole attribute is rejected") {
    const NtfsVolume<S> volume(
        std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNegativeLcn(
                FakeRunHost::Data)));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(Attr::Type::Data).empty());
  }

  SECTION("recovering: the attribute is kept, with no run mapped") {
    const NtfsVolume<S> volume(
        std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNegativeLcn(
                FakeRunHost::Data)),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK(record.ParseAttrs());

    const auto& data_attrs = record.GetAttr(Attr::Type::Data);
    REQUIRE(data_attrs.size() == 1);

    // The only run failed to decode, so cluster 0 is unmapped.
    std::vector<BYTE> buffer(NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
    // The REQUIRE above checks the size of dataAttrs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK_FALSE(data_attrs[0]->ReadData(0, buffer).has_value());
  }
}
