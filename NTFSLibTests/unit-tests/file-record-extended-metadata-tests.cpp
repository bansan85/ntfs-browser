#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
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

namespace {

// mft_tree_report_idx's $STANDARD_INFORMATION carries READONLY | ARCHIVE and,
// per WriteStandardInformationAttr(), four distinct timestamps.
template <Strategy S>
void RunFileRecordExposesExtendedMetadata() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> report(volume);
  REQUIRE(report.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
  REQUIRE(report.ParseAttrs());

  CHECK(report.IsReadOnly());
  CHECK(report.IsArchive());

  // The unnamed $DATA's own padded allocation, not $FILE_NAME's stale
  // mft_tree_report_stale_size.
  CHECK(report.GetAllocatedSize() ==
        NtfsBrowserTests::mft_tree_report_allocated_size);

  FILETIME write_tm{};
  FILETIME create_tm{};
  FILETIME access_tm{};
  FILETIME change_tm{};
  report.GetFileTime(&write_tm, &create_tm, &access_tm, &change_tm);
  CHECK(FileTimeToTicks(change_tm) != FileTimeToTicks(write_tm));
  CHECK(FileTimeToTicks(change_tm) != FileTimeToTicks(create_tm));
  CHECK(FileTimeToTicks(change_tm) != FileTimeToTicks(access_tm));
}

// mft_tree_report_idx's own $FILE_NAME now also carries READONLY | ARCHIVE
// (WriteFileNameAttr()'s extra_flags), independently of $STANDARD_INFORMATION.
// Reached the same way FileRecord::GetFileTime() and IndexEntry itself reach
// it internally (src/file-record.cpp, src/mft-tree.cpp): this record's
// directories have no $INDEX_ROOT in this fixture, so FindSubEntry() isn't an
// option here.
template <Strategy S>
void RunFilenameExposesExtendedMetadata() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> report(volume);
  REQUIRE(report.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
  REQUIRE(report.ParseAttrs());

  const auto& file_name_attrs = report.GetAttr(AttrType::FileName);
  REQUIRE_FALSE(file_name_attrs.empty());

  const Filename* own_file_name = nullptr;
  if constexpr (S == Strategy::NoCache) {
    own_file_name = reinterpret_cast<
        const AttrFileName<AttrResidentNoCache, Strategy::NoCache>*>(
        file_name_attrs.front().get());
  } else {
    own_file_name = reinterpret_cast<
        const AttrFileName<AttrResidentFullCache, Strategy::FullCache>*>(
        file_name_attrs.front().get());
  }

  CHECK(own_file_name->IsReadOnly());
  CHECK(own_file_name->IsArchive());
  // $FILE_NAME's own alloc_size/real_size pair, read independently of
  // FileRecord::GetAllocatedSize()'s $DATA-sourced one.
  CHECK(own_file_name->GetAllocatedSize() == own_file_name->GetFileSize());
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord exposes IsArchive(), GetAllocatedSize() and the "
    "change time through GetFileTime()",
    "[file-record]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  RunFileRecordExposesExtendedMetadata<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "Filename exposes IsArchive() and GetAllocatedSize() from its own "
    "$FILE_NAME",
    "[file-record]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  RunFilenameExposesExtendedMetadata<S>();
}
