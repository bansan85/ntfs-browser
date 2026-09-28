#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr-file-name.h"
#include "attr-resident.h"
#include "corpus-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrFileName;
using NtfsBrowser::AttrResidentFullCache;
using NtfsBrowser::AttrResidentNoCache;
using NtfsBrowser::AttrType;
using NtfsBrowser::Filename;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowserTests::FileTimeToTicks;

namespace
{

// kMftTreeReportIdx's $STANDARD_INFORMATION carries READONLY | ARCHIVE and,
// per WriteStandardInformationAttr(), four distinct timestamps.
template <Strategy S>
void RunFileRecordExposesExtendedMetadata()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> report(volume);
  REQUIRE(report.ParseFileRecord(NtfsBrowserTests::kMftTreeReportIdx));
  REQUIRE(report.ParseAttrs());

  CHECK(report.IsReadOnly());
  CHECK(report.IsArchive());

  // The unnamed $DATA's own padded allocation, not $FILE_NAME's stale
  // kMftTreeReportStaleSize.
  CHECK(report.GetAllocatedSize() ==
        NtfsBrowserTests::kMftTreeReportAllocatedSize);

  FILETIME writeTm{};
  FILETIME createTm{};
  FILETIME accessTm{};
  FILETIME changeTm{};
  report.GetFileTime(&writeTm, &createTm, &accessTm, &changeTm);
  CHECK(FileTimeToTicks(changeTm) != FileTimeToTicks(writeTm));
  CHECK(FileTimeToTicks(changeTm) != FileTimeToTicks(createTm));
  CHECK(FileTimeToTicks(changeTm) != FileTimeToTicks(accessTm));
}

// kMftTreeReportIdx's own $FILE_NAME now also carries READONLY | ARCHIVE
// (WriteFileNameAttr()'s extra_flags), independently of $STANDARD_INFORMATION.
// Reached the same way FileRecord::GetFileTime() and IndexEntry itself reach
// it internally (src/file-record.cpp, src/mft-tree.cpp): this record's
// directories have no $INDEX_ROOT in this fixture, so FindSubEntry() isn't an
// option here.
template <Strategy S>
void RunFilenameExposesExtendedMetadata()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> report(volume);
  REQUIRE(report.ParseFileRecord(NtfsBrowserTests::kMftTreeReportIdx));
  REQUIRE(report.ParseAttrs());

  const auto& fileNameAttrs = report.getAttr(AttrType::FILE_NAME);
  REQUIRE_FALSE(fileNameAttrs.empty());

  const Filename* ownFileName = nullptr;
  if constexpr (S == Strategy::NO_CACHE)
  {
    ownFileName = reinterpret_cast<
        const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
        fileNameAttrs.front().get());
  }
  else
  {
    ownFileName = reinterpret_cast<
        const AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
        fileNameAttrs.front().get());
  }

  CHECK(ownFileName->IsReadOnly());
  CHECK(ownFileName->IsArchive());
  // $FILE_NAME's own alloc_size/real_size pair, read independently of
  // FileRecord::GetAllocatedSize()'s $DATA-sourced one.
  CHECK(ownFileName->GetAllocatedSize() == ownFileName->GetFileSize());
}

}  // namespace

TEST_CASE(
    "FileRecord exposes IsArchive(), GetAllocatedSize() and the "
    "change time through GetFileTime()",
    "[file-record]")
{
  RunFileRecordExposesExtendedMetadata<Strategy::NO_CACHE>();
}

TEST_CASE(
    "FileRecord exposes IsArchive(), GetAllocatedSize() and the "
    "change time through GetFileTime() (FULL_CACHE)",
    "[file-record]")
{
  RunFileRecordExposesExtendedMetadata<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "Filename exposes IsArchive() and GetAllocatedSize() from its own "
    "$FILE_NAME",
    "[file-record]")
{
  RunFilenameExposesExtendedMetadata<Strategy::NO_CACHE>();
}

TEST_CASE(
    "Filename exposes IsArchive() and GetAllocatedSize() from its own "
    "$FILE_NAME (FULL_CACHE)",
    "[file-record]")
{
  RunFilenameExposesExtendedMetadata<Strategy::FULL_CACHE>();
}
