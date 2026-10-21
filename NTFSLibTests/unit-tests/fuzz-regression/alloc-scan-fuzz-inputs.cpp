#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;
using Tests::FakeMftBitmap;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// Freed files of the scan fixture, and clusters of each one's run.
constexpr size_t deleted_file_count = 8;
constexpr DWORD deleted_run_clusters = 4;

// Bytes of the live stream of the second fixture: four clusters at LCN 40.
constexpr ULONGLONG live_stream_size = 4 * Tests::fake_cluster_size;

// Freed files, some of them over a cluster that the bitmap marks used again,
// and one over a cluster that the bad list names: a scan then resolves the
// state of every stream in one sorted batch of queries.
std::vector<BYTE> DeletedBatch() {
  Tests::FakeManyDeleted built = Tests::BuildFakeNtfsImageWithManyDeletedFiles(
      deleted_file_count, deleted_run_clusters);
  std::vector<FakeDataRun> allocated;
  for (size_t i = 0; i < deleted_file_count; i += 3) {
    allocated.push_back(
        {.lcn = built.lcns.at(i) + deleted_run_clusters - 1, .clusters = 1});
  }
  std::vector<BYTE> image = std::move(built.image);
  const std::vector<BYTE> bits = Tests::AllocatedBitmapFor(image, allocated);
  Tests::PutClusterBitmap(image, bits);
  const std::array<Alloc::Extent, 1> bad{
      Alloc::Extent{.lcn = built.lcns.at(1), .clusters = 1}};
  Tests::PutBadClusterList(image, bad);
  return image;
}

// A live file over four clusters, of which the bitmap marks two used. The
// first and the last are free, and the last is also on the bad list.
std::vector<BYTE> LiveStream() {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      live_stream_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, true);
  const std::array<FakeDataRun, 1> used{FakeDataRun{.lcn = 41, .clusters = 2}};
  image = Support::WithBitmap(std::move(image), used);
  const std::array<Alloc::Extent, 2> bad{
      Alloc::Extent{.lcn = 43, .clusters = 1},
      Alloc::Extent{.lcn = 100, .clusters = 2}};
  return Support::WithBadClusters(std::move(image), bad);
}

// A bitmap that covers 32 of the volume's 256 clusters, and a bad list with an
// extent inside it, one that runs across its end, and one past it.
std::vector<BYTE> ShortBitmapWithBadList() {
  constexpr size_t bitmap_bytes = 4;
  constexpr ULONGLONG volume_clusters = 256;
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  std::vector<BYTE> bits = Tests::AllocatedBitmapFor(image, {});
  bits.resize(bitmap_bytes);
  Tests::PutClusterBitmap(image, bits,
                          {.shape = Tests::FakeBitmapShape::Resident,
                           .volume_clusters = volume_clusters,
                           .pad_to_volume = false});
  const std::array<Alloc::Extent, 3> bad{
      Alloc::Extent{.lcn = 10, .clusters = 2},
      Alloc::Extent{.lcn = 30, .clusters = 6},
      Alloc::Extent{.lcn = 100, .clusters = 3}};
  return Support::WithBadClusters(std::move(image), bad);
}

// The inputs, one per way a hostile volume can be wrong about its clusters.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"alloc_scan_deleted_batch", DeletedBatch()});
  variants.push_back({"alloc_scan_live_stream", LiveStream()});
  variants.push_back({"alloc_bitmap_short_with_bad", ShortBitmapWithBadList()});
  variants.push_back(
      {"mft_bitmap_resident",
       Tests::BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap::Resident)});
  variants.push_back(
      {"mft_bitmap_non_resident",
       Tests::BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap::NonResident)});
  variants.push_back(
      {"mft_bitmap_too_short",
       Tests::BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap::TooShort)});
  variants.push_back(
      {"mft_bitmap_lying",
       Tests::BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap::Lying)});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs where the scan meets the allocation and the
// $MFT bitmap. The volume is opened recovering with deleted items shown, since
// only then does a scan resolve the data state of freed files and log a live
// stream over free clusters. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to
// write them in (the repository's NTFSLibTests/fuzz/data) and run this hidden
// test case by its tag.
TEST_CASE("write the allocation scan fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(variant.image), recovering);
  }
}
