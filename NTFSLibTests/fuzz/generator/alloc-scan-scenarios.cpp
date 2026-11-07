#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/alloc-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeDataRun;
using FakeImage::FakeMftBitmap;

// Freed files of the scan fixture, and clusters of each one's run.
constexpr std::size_t deleted_file_count = 8;
constexpr DWORD deleted_run_clusters = 4;

// Every third freed file has its last cluster marked used again.
constexpr std::size_t reused_every = 3;

// Bytes of the live stream of the second fixture: four clusters at LCN 40.
constexpr ULONGLONG live_stream_size = 4 * FakeImage::fake_cluster_size;

// Freed files, some of them over a cluster that the bitmap marks used again,
// and one over a cluster that the bad list names: a scan then resolves the
// state of every stream in one sorted batch of queries.
std::vector<BYTE> DeletedBatch() {
  FakeImage::FakeManyDeleted built =
      FakeImage::BuildFakeNtfsImageWithManyDeletedFiles(deleted_file_count,
                                                        deleted_run_clusters);
  std::vector<FakeDataRun> allocated;
  for (std::size_t i = 0; i < deleted_file_count; i += reused_every) {
    allocated.push_back(
        {.lcn = built.lcns.at(i) + deleted_run_clusters - 1, .clusters = 1});
  }
  std::vector<BYTE> image = std::move(built.image);
  const std::vector<BYTE> bits =
      FakeImage::AllocatedBitmapFor(image, allocated);
  FakeImage::PutClusterBitmap(image, bits);
  const std::array<Alloc::Extent, 1> bad{
      Alloc::Extent{.lcn = built.lcns.at(1), .clusters = 1}};
  FakeImage::PutBadClusterList(image, bad);
  return image;
}

// A live file over four clusters, of which the bitmap marks two used. The
// first and the last are free, and the last is also on the bad list.
std::vector<BYTE> LiveStream() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          live_stream_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  FakeImage::SetRecordInUse(image, FakeImage::mft_tree_stale_child_idx, true);
  const std::array<FakeDataRun, 1> used{FakeDataRun{.lcn = 41, .clusters = 2}};
  FakeImage::PutClusterBitmap(image,
                              FakeImage::AllocatedBitmapFor(image, used));
  const std::array<Alloc::Extent, 2> bad{
      Alloc::Extent{.lcn = 43, .clusters = 1},
      Alloc::Extent{.lcn = 100, .clusters = 2}};
  FakeImage::PutBadClusterList(image, bad);
  return image;
}

// A bitmap that covers 32 of the volume's 256 clusters, and a bad list with an
// extent inside it, one that runs across its end, and one past it.
std::vector<BYTE> ShortBitmapWithBadList() {
  constexpr std::size_t bitmap_bytes = 4;
  constexpr ULONGLONG volume_clusters = 256;
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  std::vector<BYTE> bits = FakeImage::AllocatedBitmapFor(image, {});
  bits.resize(bitmap_bytes);
  FakeImage::PutClusterBitmap(image, bits,
                              {.shape = FakeImage::FakeBitmapShape::Resident,
                               .volume_clusters = volume_clusters,
                               .pad_to_volume = false});
  const std::array<Alloc::Extent, 3> bad{
      Alloc::Extent{.lcn = 10, .clusters = 2},
      Alloc::Extent{.lcn = 30, .clusters = 6},
      Alloc::Extent{.lcn = 100, .clusters = 3}};
  FakeImage::PutBadClusterList(image, bad);
  return image;
}

}  // namespace

void AllocScanScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan resolve
  // the data state of freed files and log a live stream over free clusters.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("alloc_scan_deleted_batch", DeletedBatch, options);
  registry.AddRecorded("alloc_scan_live_stream", LiveStream, options);
  registry.AddRecorded("alloc_bitmap_short_with_bad", ShortBitmapWithBadList,
                       options);
  const auto mft_bitmap = [&registry, &options](const char* name,
                                                FakeMftBitmap shape) {
    registry.AddRecorded(
        name,
        [shape] { return FakeImage::BuildFakeNtfsImageWithMftBitmap(shape); },
        options);
  };
  mft_bitmap("mft_bitmap_resident", FakeMftBitmap::Resident);
  mft_bitmap("mft_bitmap_non_resident", FakeMftBitmap::NonResident);
  mft_bitmap("mft_bitmap_too_short", FakeMftBitmap::TooShort);
  mft_bitmap("mft_bitmap_lying", FakeMftBitmap::Lying);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
