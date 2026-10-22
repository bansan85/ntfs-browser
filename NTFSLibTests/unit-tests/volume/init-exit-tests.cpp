#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "memory-disk-reader.h"
#include "test-log-sink.h"
#include "volume-tree-support.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// Offset of a non-resident header's real size, a ULONGLONG.
constexpr size_t attr_real_size_field = 48;

// Offset of an attribute header's name_length BYTE.
constexpr size_t attr_name_length_field = 9;

// Offset of a non-resident header's lowest VCN.
constexpr size_t attr_lowest_vcn_field = 16;

// Offset of the major version in $VOLUME_INFORMATION, after 8 reserved bytes.
constexpr size_t volume_info_major_field = 8;

// Major version an NT4-era volume reports, below the NTFS 3.0 floor.
constexpr BYTE old_major_version = 2;

// Real size $MFT claims in the "maps fewer" case: 1 MiB, far more than the
// fake $MFT maps, so the record count is capped below it.
constexpr ULONGLONG overclaimed_mft_bytes = 1024ULL * 1024ULL;

// Record slots the overclaimed size stands for at the fake record size.
constexpr ULONGLONG overclaimed_mft_records =
    overclaimed_mft_bytes /
    NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size;

// Record number of $Volume.
constexpr ULONGLONG volume_idx = static_cast<ULONGLONG>(Mft::Idx::Volume);

// Record number of $MFT.
constexpr ULONGLONG mft_idx = static_cast<ULONGLONG>(Mft::Idx::Mft);

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume reports no version when $Volume's attributes fail to walk",
    "[init]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  // The first attribute claims the whole record, so the walk overruns it.
  Store<DWORD>(image,
               RecordOffset(volume_idx) +
                   Load<WORD>(image, RecordOffset(volume_idx) +
                                         record_first_attr_field) +
                   attr_total_size_field,
               static_cast<DWORD>(record_bytes));

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  CHECK_FALSE(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{0, 0});
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume logs the NTFS version it reads from $Volume",
                       "[init]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("version 3.x is accepted and logged") {
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()));
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetVersion().first == 3);
    CHECK_THAT(captured, ContainsSubstring("NTFS volume version: 3."));
  }

  SECTION("version 2.x is logged and rejected") {
    std::vector<BYTE> image =
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
    const size_t attr =
        FirstAttr(image, volume_idx, Attr::Type::VolumeInformation);
    Store<BYTE>(image, ResidentBody(image, attr) + volume_info_major_field,
                old_major_version);
    (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    const std::string captured =
        NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetVersion().first == old_major_version);
    CHECK_THAT(captured, ContainsSubstring("NTFS volume version: 2."));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume stays OK without a VOLUME_NAME and logs no name", "[init]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()));
  const std::string captured = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  CHECK(volume.IsVolumeOK());
  CHECK_THAT(captured, !ContainsSubstring("NTFS volume name"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume warns when $MFT claims more records than it maps", "[init]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  Store<ULONGLONG>(
      image, FirstAttr(image, mft_idx, Attr::Type::Data) + attr_real_size_field,
      overclaimed_mft_bytes);
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  const std::string captured = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetRecordsCount() < overclaimed_mft_records);
  CHECK_THAT(captured,
             ContainsSubstring("$MFT claims 1048576 bytes but maps fewer"));
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a $MFT DATA attribute that does not start at VCN 0",
    "[init]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  Store<ULONGLONG>(
      image,
      FirstAttr(image, mft_idx, Attr::Type::Data) + attr_lowest_vcn_field, 1);

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  CHECK_FALSE(volume.IsVolumeOK());
  CHECK(volume.GetRecordsCount() == 0);
}
