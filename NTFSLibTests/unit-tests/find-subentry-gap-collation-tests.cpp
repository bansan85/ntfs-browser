#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// FindSubEntry() must descend into a real sub-node when the search name
// sorts past its parent entry only under NTFS' real collation order.
template <Strategy S>
void RunFindSubEntryDescendsIntoGapCollationSubNode()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithGapCollationSubNode());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::kGapCollationSearchName);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::kGapCollationLeafMftRef);
}

}  // namespace

TEST_CASE(
    "FindSubEntry descends into a real sub-node across the Z-a collation gap",
    "[file-record][filename][regression]")
{
  RunFindSubEntryDescendsIntoGapCollationSubNode<Strategy::NO_CACHE>();
}

TEST_CASE(
    "FindSubEntry descends into a real sub-node across the Z-a collation "
    "gap (FULL_CACHE)",
    "[file-record][filename][regression]")
{
  RunFindSubEntryDescendsIntoGapCollationSubNode<Strategy::FULL_CACHE>();
}
