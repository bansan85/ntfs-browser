#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "scripted-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;

namespace {

// Clusters the bitmap of the throwing-reader fixture marks allocated.
const std::vector<FakeDataRun> allocated_runs{
    FakeDataRun{.lcn = 40, .clusters = 2}};

// Record number of $BadClus.
constexpr ULONGLONG bad_clus_record = 8;

// Opens image, which has the 26 record slots of the MftTree base, over a
// reader that script can bend.
template <Cache::Strategy S, typename Script>
std::unique_ptr<NtfsVolume<S>> OpenScripted(std::vector<BYTE> image,
                                            Script script) {
  auto reader = std::make_unique<Tests::ScriptedDiskReader>(std::move(image));
  script(*reader);
  return std::make_unique<NtfsVolume<S>>(std::move(reader),
                                         Support::DeletedOptions());
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "a volume whose $MFT lacks the record answers unknown, not an error",
    "[alloc][volume]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(Tests::BuildFakeNtfsImage()),
      Support::DeletedOptions());
  REQUIRE(volume.IsVolumeOK());

  CHECK(volume.GetClusterState(0) == ClusterState::Unknown);
  CHECK_FALSE(volume.GetSpaceSummary());
  CHECK(volume.GetBadClusters() == nullptr);
}

TEMPLATE_TEST_CASE_SIG("a $Bitmap record without an unnamed $DATA is unusable",
                       "[alloc][volume]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Record 6 is a directory there.
  const NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(
          Tests::BuildFakeNtfsImageWithAttributeListDirectory()),
      Support::DeletedOptions());
  REQUIRE(volume.IsVolumeOK());

  CHECK(volume.GetClusterState(0) == ClusterState::Unknown);
  CHECK_FALSE(volume.GetSpaceSummary());
}

TEMPLATE_TEST_CASE_SIG("a bitmap read that throws leaves the clusters unknown",
                       "[alloc][volume]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> image = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedFileTree(), allocated_runs);
  const ULONGLONG image_size = image.size();
  const auto volume = OpenScripted<S>(
      std::move(image), [image_size](Tests::ScriptedDiskReader& reader) {
        reader.FaultRange(Support::bitmap_data_offset, image_size,
                          Tests::ScriptedDiskReader::Fault::Throw);
      });
  REQUIRE(volume->IsVolumeOK());

  CHECK_NOTHROW(volume->GetClusterState(40));
  CHECK(volume->GetClusterState(40) == ClusterState::Unknown);
}

TEST_CASE("a record read that throws leaves the bad cluster list unusable",
          "[alloc][volume]") {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithMftTree();
  const ULONGLONG mft_offset =
      NtfsVolume<Cache::Strategy::NoCache>(
          std::make_unique<Tests::MemoryDiskReader>(image))
          .GetMFTAddr();
  const ULONGLONG record_offset =
      mft_offset + bad_clus_record * Tests::fake_file_record_size;

  const auto volume = OpenScripted<Cache::Strategy::NoCache>(
      std::move(image), [record_offset](Tests::ScriptedDiskReader& reader) {
        reader.FaultRange(record_offset,
                          record_offset + Tests::fake_file_record_size,
                          Tests::ScriptedDiskReader::Fault::Throw);
      });
  REQUIRE(volume->IsVolumeOK());

  CHECK_NOTHROW(volume->GetBadClusters());
  CHECK(volume->GetBadClusters() == nullptr);
}
