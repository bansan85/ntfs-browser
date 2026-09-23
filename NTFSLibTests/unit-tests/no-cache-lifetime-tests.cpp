#include <filesystem>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

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

}  // namespace

TEST_CASE(
    "A second FileRecord's read does not corrupt $MFT's attribute (NO_CACHE)",
    "[ntfs-volume][regression]")
{
  TempImage image;

  NtfsVolume<Strategy::NO_CACHE> volume(image.path.wstring());
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  FileRecord<Strategy::NO_CACHE> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}

TEST_CASE(
    "A second FileRecord's read does not corrupt $MFT's attribute "
    "(FULL_CACHE)",
    "[ntfs-volume][regression]")
{
  TempImage image;

  NtfsVolume<Strategy::FULL_CACHE> volume(image.path.wstring());
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  FileRecord<Strategy::FULL_CACHE> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}

TEST_CASE(
    "A second FileRecord's read does not corrupt $MFT's attribute "
    "(NO_CACHE, in-memory volume)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  FileRecord<Strategy::NO_CACHE> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}
