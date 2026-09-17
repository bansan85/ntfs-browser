#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

namespace
{

// A FindSubEntry() result from $INDEX_ROOT must stay valid independent of
// the FileRecord it came from, even after that object is reparsed in place.
template <Strategy S>
void RunFindSubEntryOutlivesReparseTest()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kIndexRootVariantADirIdx));
  REQUIRE(record.ParseAttrs());

  std::optional<IndexEntry> savedEntry =
      record.FindSubEntry(NtfsBrowserTests::kIndexRootVariantAName);
  REQUIRE(savedEntry.has_value());
  CHECK(savedEntry->GetFileReference() ==
        NtfsBrowserTests::kIndexRootVariantAMftRef);
  CHECK(savedEntry->GetFilename() == NtfsBrowserTests::kIndexRootVariantAName);

  // Reparse the SAME FileRecord object for variant B's record - same fixed
  // size (kFakeFileRecordSize), so record_buffer_ is reused/overwritten in
  // place, not reallocated.
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kIndexRootVariantBDirIdx));
  REQUIRE(record.ParseAttrs());

  // The entry saved from variant A must be entirely unaffected by parsing a
  // second, different record on the same FileRecord object.
  CHECK(savedEntry->GetFileReference() ==
        NtfsBrowserTests::kIndexRootVariantAMftRef);
  CHECK(savedEntry->GetFilename() == NtfsBrowserTests::kIndexRootVariantAName);
}

}  // namespace

TEST_CASE(
    "FindSubEntry's IndexEntry from $INDEX_ROOT outlives a same-size "
    "reparse (NO_CACHE)",
    "[index-entry][regression]")
{
  RunFindSubEntryOutlivesReparseTest<Strategy::NO_CACHE>();
}

TEST_CASE(
    "FindSubEntry's IndexEntry from $INDEX_ROOT stays correct across a "
    "reparse (FULL_CACHE)",
    "[index-entry][regression]")
{
  RunFindSubEntryOutlivesReparseTest<Strategy::FULL_CACHE>();
}
