#include <ntfs-browser/win-types.h>

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "scripted-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// A record number whose byte offset in $MFT does not fit a LONGLONG.
constexpr ULONGLONG unaddressable_record =
    std::numeric_limits<ULONGLONG>::max();

// A size that every candidate sector size divides, and that leaves no
// LONGLONG address for the last sector.
constexpr ULONGLONG size_past_longlong =
    std::numeric_limits<ULONGLONG>::max() - 4095;

// Opens the backup-boot-sector fixture, whose sector 0 is zeroed, in recovery
// mode over a reader that script can bend.
template <Cache::Strategy S, typename Script>
std::unique_ptr<NtfsVolume<S>> OpenWithZeroedSector(Script script) {
  auto reader = std::make_unique<Tests::ScriptedDiskReader>(
      Tests::BuildFakeNtfsImageWithBackupBootSector());
  script(*reader);
  return std::make_unique<NtfsVolume<S>>(std::move(reader),
                                         VolumeOptions{.recover_errors = true});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("ReadMftSlots refuses a slot it cannot address",
                       "[ntfs-volume][mft-slots]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  std::vector<BYTE> out(Tests::fake_file_record_size);

  SECTION("a system record is served one by one") {
    CHECK_FALSE(volume.ReadMftSlots(0, out));
  }
  SECTION("a record number whose offset overflows") {
    CHECK_FALSE(volume.ReadMftSlots(unaddressable_record, out));
  }
  SECTION("a user record is served") {
    CHECK(volume.ReadMftSlots(Tests::mft_tree_docs_idx, out));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "the backup boot sector is skipped when its address does not exist",
    "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto volume = OpenWithZeroedSector<S>(
      [](Tests::ScriptedDiskReader& r) { r.DeclareSize(size_past_longlong); });

  CHECK_FALSE(volume->IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "the backup boot sector is skipped when its sector cannot be read",
    "[ntfs-volume][backup-boot]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto volume = OpenWithZeroedSector<S>([](Tests::ScriptedDiskReader& r) {
    // Every candidate sector ends at the end of the image.
    r.FaultRange(Tests::fake_backup_image_bytes - 512,
                 Tests::fake_backup_image_bytes,
                 Tests::ScriptedDiskReader::Fault::Fail);
  });

  CHECK_FALSE(volume->IsVolumeOK());
}

#ifdef _WIN32
TEMPLATE_TEST_CASE_SIG(
    "a volume opened by path reads ahead when the options ask for it",
    "[ntfs-volume][read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::filesystem::path path = Tests::WriteFakeNtfsImage();

  {
    const NtfsVolume<S> volume(path.wstring(),
                               VolumeOptions{.read_ahead_bytes = 4096});
    CHECK(volume.IsVolumeOK());
  }

  std::error_code error;
  std::filesystem::remove(path, error);
}
#endif
