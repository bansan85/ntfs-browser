#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Alloc {

struct Extent;

}  // namespace Alloc

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Tests {

class CountingDiskReader;
class MemoryDiskReader;

}  // namespace Tests
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::AllocSupport {

// First cluster of the bitmap data an image gets from PutClusterBitmap() when
// the image is the 64 KiB base: the clusters before it are the volume's own.
inline constexpr ULONGLONG base_volume_clusters = 64;

// First byte of the bitmap data in such an image.
inline constexpr ULONGLONG bitmap_data_offset =
    base_volume_clusters * fake_cluster_size;

// Bytes of the stale stream the freed non-resident fixture keeps: four
// clusters.
inline constexpr ULONGLONG stale_size = 4 * fake_cluster_size;

// The volume opened over an image, and the counting reader under it.
template <Cache::Strategy S>
struct Opened {
  std::unique_ptr<NtfsVolume<S>> volume;
  // Owned by volume.
  CountingDiskReader* counter{nullptr};
};

// Options that list freed records, and nothing else.
inline VolumeOptions DeletedOptions() {
  return VolumeOptions{.include_deleted = true};
}

// Opens image as a volume whose reader counts every call.
template <Cache::Strategy S>
Opened<S> OpenVolume(std::vector<BYTE> image,
                     const VolumeOptions& options = DeletedOptions()) {
  auto counter = std::make_unique<CountingDiskReader>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  Opened<S> opened;
  opened.counter = counter.get();
  opened.volume = std::make_unique<NtfsVolume<S>>(std::move(counter), options);
  REQUIRE(opened.volume->IsVolumeOK());
  return opened;
}

// image with a non-resident $Bitmap that marks exactly allocated_runs, and the
// clusters of the bitmap itself.
inline std::vector<BYTE> WithBitmap(std::vector<BYTE> image,
                                    std::span<const FakeDataRun> allocated_runs,
                                    const FakeBitmapOptions& options = {}) {
  const std::vector<BYTE> bits = AllocatedBitmapFor(image, allocated_runs);
  PutClusterBitmap(image, bits, options);
  return image;
}

// image with record 8 rewritten as $BadClus naming bad_extents. Call it after
// WithBitmap(): the bitmap grows the image, and $Bad declares its size.
inline std::vector<BYTE>
    WithBadClusters(std::vector<BYTE> image,
                    std::span<const Alloc::Extent> bad_extents,
                    const FakeBadClusterOptions& options = {}) {
  PutBadClusterList(image, bad_extents, options);
  return image;
}

// allocated plus the runs of extents: a bitmap that marks the bad clusters
// allocated, as NTFS does.
inline std::vector<FakeDataRun>
    WithBadRuns(std::vector<FakeDataRun> allocated,
                std::span<const Alloc::Extent> extents) {
  for (const FakeDataRun& run : ExtentsAsRuns(extents)) {
    allocated.push_back(run);
  }
  return allocated;
}

// A file record parsed and its attributes read, from a volume.
template <Cache::Strategy S>
std::unique_ptr<Io::FileRecord<S>> ParsedRecord(const NtfsVolume<S>& volume,
                                                ULONGLONG idx) {
  auto record = std::make_unique<Io::FileRecord<S>>(volume);
  REQUIRE(record->ParseFileRecord(idx));
  REQUIRE(record->ParseAttrs());
  return record;
}

// The bad list of volume, which the test cannot go on without.
template <Cache::Strategy S>
const Alloc::BadClusters& RequiredBadList(const NtfsVolume<S>& volume) {
  const Alloc::BadClusters* list = volume.GetBadClusters();
  REQUIRE(list != nullptr);
  return *list;
}

// The stale file's record, parsed with its attributes.
template <Cache::Strategy S>
std::unique_ptr<Io::FileRecord<S>> ParsedStale(const Opened<S>& opened) {
  return ParsedRecord(*opened.volume, mft_tree_stale_child_idx);
}

// Reads the whole stale stream.
template <Cache::Strategy S>
std::vector<BYTE> ReadStale(const Opened<S>& opened) {
  const auto record = ParsedStale(opened);
  std::vector<BYTE> out(stale_size);
  const auto read = record->FindStream({})->ReadData(0, out);
  REQUIRE(read);
  REQUIRE(*read == stale_size);
  return out;
}

// Opens image, reads the whole stale stream, and returns what the library
// logged from then on.
template <Cache::Strategy S>
std::string LogOfReadingStale(std::vector<BYTE> image,
                              const VolumeOptions& options) {
  const auto opened = OpenVolume<S>(std::move(image), options);
  (void)TakeCapturedLog();
  (void)ReadStale(opened);
  return TakeCapturedLog();
}

// Fills the stale stream's four clusters, at LCN 40, with a pattern of the
// given step, so a read returns something to compare.
inline void FillStaleClusters(std::vector<BYTE>& image, size_t step) {
  for (size_t i = 0; i < stale_size; i++) {
    image.at(40 * fake_cluster_size + i) = static_cast<BYTE>(i * step);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::AllocSupport
