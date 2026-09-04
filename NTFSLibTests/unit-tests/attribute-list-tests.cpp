#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression test for a bug in FileRecord::SetAttrMask(): it let a caller's
// mask silently exclude $ATTRIBUTE_LIST, even though the comment right
// above the assignment says "Standard Information and Attribute List is
// needed always". Real directories that outgrow their base MFT record (eg.
// C:\Windows) relocate $INDEX_ROOT/$INDEX_ALLOCATION into an extension
// record and leave only an $ATTRIBUTE_LIST pointer behind. NtfsDir.exe asks
// for Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION only, so that pointer was
// never followed and such directories silently listed as empty - eg.
// ".\NtfsDir.exe C:\windows" printed "Files: 0, Directories: 0" instead of
// C:\Windows' real contents.
//
// BuildFakeNtfsImageWithAttributeListDirectory() reproduces the same shape:
// kAttributeListDirIdx's only attribute is a resident $ATTRIBUTE_LIST
// pointing $INDEX_ROOT at kIndexExtensionIdx, which holds the actual
// "Foo" entry.
TEST_CASE("FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
          "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  // Mirrors NtfsDir.exe's actual call: it asks for the two index
  // attributes only, not $ATTRIBUTE_LIST.
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());

  // Before the fix, $ATTRIBUTE_LIST was never parsed, so $INDEX_ROOT -
  // which only exists in the extension record - never got merged in here.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);
}

// Regression test for F17's primary defect (docs/bug-reports/2026-09-03-full-repo.md):
// AttrList<S>::AttrList() (src/attr-list.cpp) deduplicates the set of
// resolved extension records (fr.attr_list_chain_) on record_ref alone, not
// on (record_ref, attr_type). A single extension record commonly hosts
// several relocated attributes of different types (eg. $INDEX_ROOT and
// $INDEX_ALLOCATION, as real directories like C:\Windows do once they
// outgrow their base record) - once the first $ATTRIBUTE_LIST entry
// resolves that record, every later entry pointing at the very same record
// is skipped outright, regardless of which attribute type it names.
//
// BuildFakeNtfsImageWithMultiTypeAttributeListDirectory() reproduces this:
// kAttrListMultiTypeDirIdx's $ATTRIBUTE_LIST has two entries, both naming
// kMultiTypeExtensionIdx - first for $INDEX_ROOT, then for
// $INDEX_ALLOCATION - and that single extension record holds both
// attributes.
TEST_CASE(
    "AttrList merges every attribute type relocated into the same "
    "extension record (F17)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMultiTypeAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListMultiTypeDirIdx));
  REQUIRE(dir.ParseAttrs());

  // The first $ATTRIBUTE_LIST entry (relocating $INDEX_ROOT) is always
  // resolved fine, first-in-chain.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Before the fix: the second entry, relocating $INDEX_ALLOCATION to the
  // SAME extension record, gets skipped because that record_ref was
  // already marked resolved by the first entry - even though
  // $INDEX_ALLOCATION itself was never actually retrieved.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ALLOCATION).empty());
}

// Regression test for F17's secondary defect
// (docs/bug-reports/2026-09-03-full-repo.md): FileRecord::attr_list_chain_
// is lazily created but never reset by FileRecord::ParseFileRecord() (only
// ClearAttrs() + file_record_.reset() run there). Sample apps like
// ntfsdir.exe and ntfsundel.exe reuse a single FileRecord across many
// ParseFileRecord()/ParseAttrs() calls while walking a volume, so the set
// of "already resolved" extension records from one file's $ATTRIBUTE_LIST
// chain silently leaks into the next, unrelated file's chain.
//
// BuildFakeNtfsImageWithAttributeListDirectoryChainReused() reproduces
// this: a second, independent directory (kAttributeListDirIdx2) whose own
// $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx - the
// very same extension record the first directory
// (kAttributeListDirIdx) already resolved via the same, reused
// FileRecord object.
TEST_CASE(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord (F17)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListDirectoryChainReused());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  // First parse: a fresh FileRecord resolves $INDEX_ROOT through
  // kIndexExtensionIdx normally.
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Second parse, same FileRecord object, unrelated directory whose own
  // $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx.
  // Before the fix: attr_list_chain_ survives from the first parse still
  // containing kIndexExtensionIdx, so this second, independent resolution
  // is wrongly skipped as "already resolved".
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx2));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}
