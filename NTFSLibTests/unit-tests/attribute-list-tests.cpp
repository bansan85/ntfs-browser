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
TEST_CASE(
    "FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
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
