#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "partition-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

struct TempImage final {
  std::filesystem::path path =
      NTFS_BROWSER_NAMESPACE::Tests::WriteFakeNtfsImage();
  TempImage() = default;

  ~TempImage() { std::filesystem::remove(path); }

  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
  TempImage(TempImage&&) = delete;
  TempImage& operator=(TempImage&&) = delete;
};

// Opens path through a PartitionDiskReader (offset 0), so this exercises a
// real on-disk file read without depending on NtfsVolume's own path-based
// constructor, which only exists on Windows (Win32DiskReader).
std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>
    OpenOnDisk(const std::filesystem::path& path) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return reader;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A second FileRecord's read does not corrupt $MFT's attribute",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const TempImage image;

  const NtfsVolume<S> volume(OpenOnDisk(image.path));
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() ==
          NTFS_BROWSER_NAMESPACE::Tests::sentinel_record_count);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

  CHECK(volume.GetRecordsCount() ==
        NTFS_BROWSER_NAMESPACE::Tests::sentinel_record_count);
}

TEMPLATE_TEST_CASE_SIG(
    "A second FileRecord's read does not corrupt $MFT's attribute "
    " (in-memory volume)",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() ==
          NTFS_BROWSER_NAMESPACE::Tests::sentinel_record_count);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

  CHECK(volume.GetRecordsCount() ==
        NTFS_BROWSER_NAMESPACE::Tests::sentinel_record_count);
}
