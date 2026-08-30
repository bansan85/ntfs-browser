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

// Regression test for the use-after-free fixed in FileReader<NO_CACHE>: it
// used to keep a single reusable read buffer shared by every FileRecord on
// the volume, including NtfsVolume::mft_record_ ($MFT's own record, needed
// for the volume's whole lifetime). Reading any other record through the
// same "direct disk allocation" path silently overwrote that memory, so the
// $MFT DATA attribute's cached header (mft_data_, used by GetRecordsCount())
// got corrupted by a read that had nothing to do with it.
TEST_CASE(
    "A second FileRecord's read does not corrupt $MFT's attribute (NO_CACHE)",
    "[ntfs-volume][regression]")
{
  TempImage image;

  NtfsVolume<Strategy::NO_CACHE> volume(image.path.wstring());
  REQUIRE(volume.IsVolumeOK());
  REQUIRE(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);

  // Parses the root directory's record through the same "direct disk
  // allocation" path $MFT's own record used to populate mft_data_.
  FileRecord<Strategy::NO_CACHE> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK(volume.GetRecordsCount() == NtfsBrowserTests::kSentinelRecordCount);
}

// Same scenario under FULL_CACHE, which always copies attribute data out of
// FileReader's buffer: kept here as a sanity check that the fake image and
// the assertions above are meaningful for both strategies, not an artifact
// of one code path.
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

// Same regression, but backed by MemoryDiskReader instead of a temp file -
// exercises NtfsVolume's reader-injection constructor without any disk I/O.
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
