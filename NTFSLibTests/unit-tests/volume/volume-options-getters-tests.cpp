#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithDeletedVolumeRecord;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// VolumeOptions{} default-constructed: both flags off.
template <Cache::Strategy S>
void RunDefaultsAreBothOff() {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());
  CHECK_FALSE(volume.GetOptions().include_deleted);
  CHECK_FALSE(volume.GetOptions().recover_errors);
}

// GetOptions() reflects exactly what the constructor was given.
template <Cache::Strategy S>
void RunGetOptionsReflectsConstructorArgument() {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImage()),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetOptions().include_deleted);
  CHECK(volume.GetOptions().recover_errors);
}

// VG6: bypass_deleted_gate_ lets NtfsVolume::Init() read its own internal
// $Volume FileRecord even when that record is freed, under default
// VolumeOptions (include_deleted off) - unlike an ordinary file record,
// which the deleted gate would empty out.
template <Cache::Strategy S>
void RunDeletedVolumeRecordStillOpens() {
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      BuildFakeNtfsImageWithDeletedVolumeRecord()));
  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("VolumeOptions default to both flags off",
                       "[volume-options]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunDefaultsAreBothOff<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume::GetOptions reflects the constructor argument",
    "[volume-options]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RunGetOptionsReflectsConstructorArgument<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "A freed $Volume record still opens the volume under default "
    "VolumeOptions",
    "[volume-options][ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunDeletedVolumeRecordStillOpens<S>();
}
