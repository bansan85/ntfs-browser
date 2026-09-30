#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "partition-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

struct TempImage
{
  std::filesystem::path path = NtfsBrowserTests::WriteFakeNtfsImage();
  TempImage() = default;
  ~TempImage() { std::filesystem::remove(path); }
  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
};

// Opens path through a PartitionDiskReader (offset 0), so this exercises a
// real on-disk file read without depending on NtfsVolume's own path-based
// constructor, which only exists on Windows (Win32DiskReader).
std::unique_ptr<NtfsBrowser::IDiskReader>
    OpenOnDisk(const std::filesystem::path& path)
{
  auto reader = std::make_unique<NtfsBrowserTests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return reader;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A second FileRecord's read does not corrupt $MFT's attribute",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  TempImage image;

  NtfsVolume<S> volume(OpenOnDisk(image.path));
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}

TEMPLATE_TEST_CASE_SIG(
    "A second FileRecord's read does not corrupt $MFT's attribute "
    " (in-memory volume)",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}
