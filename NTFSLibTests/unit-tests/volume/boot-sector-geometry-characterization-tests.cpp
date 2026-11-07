#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/volume/volume-tree-support.h>

#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Test::DiskReaders {

class MemoryDiskReader;

}  // namespace Test::DiskReaders
}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader;

namespace {

// BuildFakeNtfsImage() with edit applied to its boot sector BPB.
template <typename Edit>
std::vector<BYTE> ImageWithBpb(Edit edit) {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  return image;
}

// The volume opened over BuildFakeNtfsImage() with edit applied to its BPB.
template <Cache::Strategy S, typename Edit>
NtfsVolume<S> VolumeWithBpb(Edit edit) {
  return NtfsVolume<S>(std::make_unique<MemoryDiskReader>(ImageWithBpb(edit)));
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

// The values a case expects from the volume getters. A field left empty is
// not checked.
struct ExpectedGeometry {
  std::optional<ULONGLONG> sector_size;
  std::optional<ULONGLONG> cluster_size;
  std::optional<ULONGLONG> file_record_size;
  std::optional<ULONGLONG> index_block_size;
  std::optional<ULONGLONG> mft_addr;
  std::optional<ULONGLONG> cluster_buffer_size;
};

// A BPB edit and the geometry the volume keeps after it.
struct GeometryCase {
  std::string name;
  std::function<void(NTFS_BROWSER_NAMESPACE::Data::NtfsBpb&)> edit;
  std::optional<bool> ok;
  ExpectedGeometry expected;
};

// The value the getter returned, when the case names one.
void CheckGeometryField(const char* field,
                        const std::optional<ULONGLONG>& expected,
                        ULONGLONG actual) {
  if (expected) {
    INFO(field);
    CHECK(actual == *expected);
  }
}

template <Cache::Strategy S>
void CheckGeometry(const NtfsVolume<S>& volume,
                   const ExpectedGeometry& expected) {
  CheckGeometryField("sector size", expected.sector_size,
                     volume.GetSectorSize());
  CheckGeometryField("cluster size", expected.cluster_size,
                     volume.GetClusterSize());
  CheckGeometryField("file record size", expected.file_record_size,
                     volume.GetFileRecordSize());
  CheckGeometryField("index block size", expected.index_block_size,
                     volume.GetIndexBlockSize());
  CheckGeometryField("MFT address", expected.mft_addr, volume.GetMFTAddr());
  CheckGeometryField("cluster buffer size", expected.cluster_buffer_size,
                     volume.GetClusterBuffer().size());
}

std::vector<GeometryCase> GeometryCases() {
  using NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_bytes_per_sector;
  using NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size;
  using NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_file_record_size;
  return {
      {.name = "a bad NTFS signature leaves every value zero",
       .edit = [](auto& bpb) { bpb.signature[0] = 'X'; },
       .ok = false,
       .expected = {.sector_size = 0,
                    .cluster_size = 0,
                    .cluster_buffer_size = 0}},
      {.name = "a null cluster keeps the sector size and an empty cluster "
               "buffer",
       .edit = [](auto& bpb) { bpb.sectors_per_cluster = 0; },
       .ok = false,
       .expected = {.sector_size = fake_bytes_per_sector,
                    .cluster_size = 0,
                    .file_record_size = 0,
                    .cluster_buffer_size = 0}},
      {.name = "a file record exponent out of range keeps the cluster buffer "
               "sized",
       .edit =
           [](auto& bpb) {
             bpb.clusters_per_file_record =
                 static_cast<DWORD>(exponent_past_limit);
           },
       .ok = false,
       .expected = {.sector_size = fake_bytes_per_sector,
                    .cluster_size = fake_cluster_size,
                    .file_record_size = 0,
                    .index_block_size = 0,
                    .mft_addr = 0,
                    .cluster_buffer_size = fake_cluster_size}},
      {.name = "a file record above the maximum keeps its decoded size",
       .edit =
           [](auto& bpb) {
             bpb.clusters_per_file_record = largest_literal_count;
           },
       .ok = false,
       .expected = {.file_record_size = 8 * fake_cluster_size,
                    .index_block_size = 0,
                    .mft_addr = 0}},
      {.name = "an index block exponent out of range keeps the file record "
               "size",
       .edit =
           [](auto& bpb) {
             bpb.clusters_per_index_block = literal_count_past_limit;
           },
       .ok = false,
       .expected = {.file_record_size = fake_file_record_size,
                    .index_block_size = 0,
                    .mft_addr = 0}},
      {.name = "an overflowing MFT address keeps the decoded block sizes",
       .edit =
           [](auto& bpb) {
             bpb.lcn_mft =
                 std::numeric_limits<ULONGLONG>::max() / fake_cluster_size + 1;
           },
       .ok = false,
       .expected = {.file_record_size = fake_file_record_size,
                    .index_block_size = fake_cluster_size,
                    .mft_addr = std::numeric_limits<ULONGLONG>::max()}},
      {.name = "eight clusters per index block is accepted at its full size",
       .edit =
           [](auto& bpb) {
             bpb.clusters_per_index_block = largest_literal_count;
           },
       .expected = {.index_block_size = 8 * fake_cluster_size}},
      // The image is far smaller than one cluster, so the $MFT read fails and
      // IsVolumeOK() stays false. The decoded values still hold.
      {.name = "a 2 MiB cluster sizes every value and the cluster buffer",
       .edit =
           [](auto& bpb) {
             bpb.sectors_per_cluster =
                 static_cast<BYTE>(largest_cluster_exponent);
             bpb.clusters_per_file_record =
                 static_cast<DWORD>(one_kib_record_exponent);
             bpb.clusters_per_index_block = 1;
           },
       .expected = {.cluster_size = two_mib_cluster_size,
                    .file_record_size = 1024,
                    .index_block_size = two_mib_cluster_size,
                    .mft_addr = two_mib_cluster_size,
                    .cluster_buffer_size = two_mib_cluster_size}},
  };
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume keeps the geometry decoded before a rejected "
    "field",
    "[ntfs-volume][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("a refused boot sector read leaves every value zero") {
    const auto refused = std::make_shared<size_t>(0);
    const NtfsVolume<S> volume(std::make_unique<RefusingDiskReader>(
        NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage(), 0, 1,
        refused));

    CHECK_FALSE(volume.IsVolumeOK());
    CheckGeometry(volume, {.sector_size = 0,
                           .cluster_size = 0,
                           .file_record_size = 0,
                           .index_block_size = 0,
                           .mft_addr = 0,
                           .cluster_buffer_size = 0});
  }

  for (const GeometryCase& geometry_case : GeometryCases()) {
    DYNAMIC_SECTION(geometry_case.name) {
      const NtfsVolume<S> volume = VolumeWithBpb<S>(geometry_case.edit);

      if (geometry_case.ok) {
        CHECK(volume.IsVolumeOK() == *geometry_case.ok);
      }
      CheckGeometry(volume, geometry_case.expected);
    }
  }
}
