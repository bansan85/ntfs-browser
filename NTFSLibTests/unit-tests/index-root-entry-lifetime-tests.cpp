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

// Regression test for F21 (docs/bug-reports/2026-09-03-full-repo.md):
// AttrIndexRoot<RESIDENT, S>::ParseIndexEntries() (src/attr-index-root.cpp)
// used to emplace_back() every IndexEntry with a NULL shared_ptr<BYTE[]>
// instead of an owned copy of its own resident attribute data - unlike
// AttrIndexAlloc<S>::ParseIndexBlock() (src/attr-index-alloc.cpp), which
// already allocates and shares one. FileRecord<S>::FindSubEntry()
// (src/file-record.cpp) hands such an IndexEntry back "by value" under a
// documented "Must be a copy" contract, but without independent backing
// memory the copy still aliases the FileRecord/AttrIndexRoot object's own
// storage: a real use-after-free once that object is destroyed/reparsed
// (FULL_CACHE's AttrResidentFullCache::body_, a std::vector<BYTE> member),
// or, deterministically under NO_CACHE, silent staleness once
// FileRecord::record_buffer_ is overwritten in place by a same-size second
// read (AttrResidentNoCache::body_ is only a std::span into it, and
// ReadFileRecord() only resizes record_buffer_ if the size differs -
// BuildFakeNtfsImageWithIndexRootVariants()'s two directory records are both
// exactly kFakeFileRecordSize).
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

// Same assertions under FULL_CACHE: this strategy's failure mode pre-fix is
// a genuine use-after-free (AttrResidentFullCache::body_ is destroyed
// outright, not merely overwritten), which is unsafe/non-deterministic to
// provoke reliably in a unit test - this only confirms the fix's correct
// behavior here too, mirroring the NO_CACHE assertions above.
TEST_CASE(
    "FindSubEntry's IndexEntry from $INDEX_ROOT stays correct across a "
    "reparse (FULL_CACHE)",
    "[index-entry][regression]")
{
  RunFindSubEntryOutlivesReparseTest<Strategy::FULL_CACHE>();
}
