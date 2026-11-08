#include "cluster-bitmap-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "bpb-edit.h"
#include "data/ntfs-bpb.h"
#include "resource-locator.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeBitmapOptions;
using FakeImage::FakeBitmapShape;
using FakeImage::FakeDataRun;

// The stale file that every input keeps: four clusters at LCN 40.
constexpr ULONGLONG stale_size = 4 * FakeImage::fake_cluster_size;
constexpr FakeDataRun stale_run{.lcn = 40, .clusters = 4};

// The one cluster the bitmap of most inputs marks used.
constexpr FakeDataRun marked_run{.lcn = 42, .clusters = 1};

// Bytes of a bitmap that needs two clusters: one more than a cluster holds, so
// the data takes the second.
constexpr std::size_t two_cluster_bitmap_bytes = 1100;

// A size no volume has: 2^50 bytes of bitmap.
constexpr ULONGLONG forged_bitmap_size = ULONGLONG{1} << 50U;

// The freed tree with a freed file that keeps four clusters of data.
std::vector<BYTE> StaleTree() {
  return FakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size,
                                                               {stale_run});
}

// The stale tree with a $Bitmap of shape that marks marked_run, written as
// options say.
std::vector<BYTE> WithMarkedBitmap(const FakeBitmapOptions& options) {
  std::vector<BYTE> image = StaleTree();
  const std::array<FakeDataRun, 1> marked{marked_run};
  FakeImage::PutClusterBitmap(
      image, FakeImage::AllocatedBitmapFor(image, marked), options);
  return image;
}

// The stale tree with a $Bitmap of two clusters of zero bits, whose run list
// stops after the first cluster.
std::vector<BYTE> CutRunList() {
  std::vector<BYTE> image = StaleTree();
  const std::vector<BYTE> bits(two_cluster_bitmap_bytes, 0);
  FakeImage::PutClusterBitmap(
      image, bits,
      {.shape = FakeBitmapShape::CutRunList, .pad_to_volume = false});
  return image;
}

// The stale tree with a resident $Bitmap of four bytes: it covers 32 of the 64
// clusters, and marks only the first.
std::vector<BYTE> ShortResident() {
  std::vector<BYTE> image = StaleTree();
  const std::vector<BYTE> bits{0x01, 0x00, 0x00, 0x00};
  FakeImage::PutClusterBitmap(
      image, bits,
      {.shape = FakeBitmapShape::Resident, .pad_to_volume = false});
  return image;
}

}  // namespace

void ClusterBitmapScenarios::Register(ScenarioRegistry& registry) {
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("alloc_bitmap_cut_run_list", CutRunList, options);
  registry.AddRecorded(
      "alloc_bitmap_fragmented",
      [] { return WithMarkedBitmap({.shape = FakeBitmapShape::Fragmented}); },
      options);
  // The bitmap is much shorter than the volume the fuzzer claims (it patches
  // the sector count of every input).
  registry.AddRecorded(
      "alloc_bitmap_huge_volume_claim", [] { return WithMarkedBitmap({}); },
      options);
  registry.AddRecorded(
      "alloc_bitmap_forged_size",
      [] { return WithMarkedBitmap({.forged_size = forged_bitmap_size}); },
      options);
  registry.AddRecorded("alloc_bitmap_short_resident", ShortResident, options);

  // The saved input kept a boot sector with no sector count and an end marker
  // whose bytes are swapped. The fuzzer repairs both before it replays.
  registry.Add({.name = "bitmap_multi_cluster",
                .make = [options](const ResourceLocator& /*resources*/) {
                  std::vector<BYTE> stream = RecordedStream::Record(
                      FakeImage::BuildFakeNtfsImageWithMultiClusterBitmap(),
                      options, RecordedStream::Probe::Main);
                  BpbEdit::Apply(stream, [](Data::NtfsBpb& bpb) {
                    bpb.total_sectors = 0;
                    bpb.x_aa = 0xAA;
                    bpb.x_55 = 0x55;
                  });
                  return stream;
                }});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
