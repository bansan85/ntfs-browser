#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "volume-tree-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// Sectors-per-cluster exponent of the largest cluster, 2 MiB at 512 bytes.
constexpr int largest_cluster_exponent = -12;

// One exponent below the -12 limit: no BPB exponent may declare it.
constexpr int exponent_past_limit = -13;

// Exponent that gives a 1 KiB file record, the fake image's own size.
constexpr int one_kib_record_exponent = -10;

// Literal cluster count above the 8-cluster limit of a BPB size field.
constexpr DWORD literal_count_past_limit = 9;

// Largest literal cluster count; 8 KiB at the fake cluster, past the record
// maximum.
constexpr DWORD largest_literal_count = 8;

// 2 MiB: the cluster that largest_cluster_exponent gives at 512-byte sectors.
constexpr DWORD two_mib_cluster_size = 2U * 1024U * 1024U;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume keeps the geometry decoded before a rejected "
    "field",
    "[ntfs-volume][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("a refused boot sector read leaves every value zero") {
    const auto refused = std::make_shared<size_t>(0);
    const NtfsVolume<S> volume(std::make_unique<RefusingDiskReader>(
        NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage(), 0, 1, refused));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() == 0);
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetFileRecordSize() == 0);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }

  SECTION("a bad NTFS signature leaves every value zero") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        ImageWithBpb([](auto& bpb) { bpb.signature[0] = 'X'; })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() == 0);
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }

  SECTION("a null cluster keeps the sector size and an empty cluster buffer") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        ImageWithBpb([](auto& bpb) { bpb.sectors_per_cluster = 0; })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
    CHECK(volume.GetClusterSize() == 0);
    CHECK(volume.GetFileRecordSize() == 0);
    CHECK(volume.GetClusterBuffer().empty());
  }

  SECTION(
      "a file record exponent out of range keeps the cluster buffer sized") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.clusters_per_file_record =
              static_cast<DWORD>(exponent_past_limit);
        })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetSectorSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_bytes_per_sector);
    CHECK(volume.GetClusterSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
    CHECK(volume.GetFileRecordSize() == 0);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
    CHECK(volume.GetClusterBuffer().size() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
  }

  SECTION("a file record above the maximum keeps its decoded size") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.clusters_per_file_record = largest_literal_count;
        })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetFileRecordSize() ==
          8 * NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
  }

  SECTION("an index block exponent out of range keeps the file record size") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.clusters_per_index_block = literal_count_past_limit;
        })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetFileRecordSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size);
    CHECK(volume.GetIndexBlockSize() == 0);
    CHECK(volume.GetMFTAddr() == 0);
  }

  SECTION("an overflowing MFT address keeps the decoded block sizes") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.lcn_mft = std::numeric_limits<ULONGLONG>::max() /
                            NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size +
                        1;
        })));

    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetFileRecordSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size);
    CHECK(volume.GetIndexBlockSize() ==
          NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
    CHECK(volume.GetMFTAddr() == std::numeric_limits<ULONGLONG>::max());
  }

  SECTION("eight clusters per index block is accepted at its full size") {
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.clusters_per_index_block = largest_literal_count;
        })));

    CHECK(volume.GetIndexBlockSize() ==
          8 * NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
  }

  SECTION("a 2 MiB cluster sizes every value and the cluster buffer") {
    // The image is far smaller than one cluster, so the $MFT read fails and
    // IsVolumeOK() stays false. The decoded values still hold.
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(ImageWithBpb([](auto& bpb) {
          bpb.sectors_per_cluster = static_cast<BYTE>(largest_cluster_exponent);
          bpb.clusters_per_file_record =
              static_cast<DWORD>(one_kib_record_exponent);
          bpb.clusters_per_index_block = 1;
        })));

    CHECK(volume.GetClusterSize() == two_mib_cluster_size);
    CHECK(volume.GetClusterBuffer().size() == two_mib_cluster_size);
    CHECK(volume.GetFileRecordSize() == 1024);
    CHECK(volume.GetIndexBlockSize() == two_mib_cluster_size);
    CHECK(volume.GetMFTAddr() == two_mib_cluster_size);
  }
}
