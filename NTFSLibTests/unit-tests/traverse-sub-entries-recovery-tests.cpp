#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Both flags on: the recovery scan runs, and Decision 4's include_deleted
// filter is bypassed, so only the pre-existing parent-reference check
// applies.
constexpr VolumeOptions kRecoverKeepDeleted{.include_deleted = true,
                                            .recover_errors = true};

// Collects every name TraverseSubEntries() reports, in callback order.
template <Strategy S>
std::vector<std::wstring> CollectNames(const FileRecord<S>& root)
{
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            ie.GetFilename());
      },
      &names);
  return names;
}

// By default, TraverseSubEntries() only follows the B+ tree's own pointers,
// so an index block no pointer reaches stays invisible.
template <Strategy S>
void RunOrphanedBlocksNeedRecoveryFlag()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> normal = CollectNames(root);
  REQUIRE(normal.size() == 1);
  CHECK(normal[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
}

// With recover_errors on and include_deleted on, every $INDEX_ALLOCATION
// block the tree walk missed is scanned too, but an entry filed under a
// different parent is rejected. include_deleted is on here because none of
// this fixture's leaf entries name a record that actually exists: with it
// off, Decision 4's filter drops every one of them instead (see
// RunOrphanedBlocksDroppedWithoutIncludeDeleted below).
template <Strategy S>
void RunOrphanedBlocksFoundWithRecoveryFlag()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader), kRecoverKeepDeleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 2);
  CHECK(recovered[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
  CHECK(recovered[1] == NtfsBrowserTests::kOrphanedBlockOrphanName);
}

// With include_deleted off (the default), Decision 4 additionally requires
// an orphan-scan entry's named record to actually exist, be in use, and
// carry a matching sequence number. Neither orphaned leaf entry (Orphan,
// Stale) names a record that exists in this fixture's tiny $MFT, so the
// orphan scan itself contributes nothing - unlike with include_deleted on
// above. Decision 4 only governs the orphan scan though: Reachable comes
// from the normal B+ tree walk and is unaffected, so it still appears.
template <Strategy S>
void RunOrphanedBlocksDroppedWithoutIncludeDeleted()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader),
                       VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 1);
  CHECK(recovered[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
}

// Decision 4's other drop condition: with include_deleted off, an
// orphan-scan entry naming a record that DOES exist and IS in use, but
// under a sequence number that does not match the entry's own, is dropped
// exactly like one naming a record that doesn't exist at all (see
// RunOrphanedBlocksDroppedWithoutIncludeDeleted above). With include_deleted
// on, Decision 4 is bypassed entirely and the entry is kept, confirming the
// drop above is specifically about the sequence mismatch.
template <Strategy S>
void RunOrphanedBlocksDroppedOnSequenceMismatch()
{
  {
    auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
        NtfsBrowserTests::
            BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch());

    NtfsVolume<S> volume(std::move(reader),
                         VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> recovered = CollectNames(root);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
  }
  {
    auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
        NtfsBrowserTests::
            BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch());

    NtfsVolume<S> volume(std::move(reader), kRecoverKeepDeleted);
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> recovered = CollectNames(root);
    REQUIRE(recovered.size() == 2);
    CHECK(recovered[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
    CHECK(recovered[1] == NtfsBrowserTests::kOrphanedBlockOrphanName);
  }
}

// With no $INDEX_ROOT at all, the normal walk has nowhere to start, so
// without the recovery flag TraverseSubEntries() reports nothing.
template <Strategy S>
void RunMissingIndexRootNeedsRecoveryFlag()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  CHECK(CollectNames(root).empty());
}

// With the recovery flag, a record with no parsed $INDEX_ROOT still yields
// every entry reachable through $INDEX_ALLOCATION alone.
template <Strategy S>
void RunMissingIndexRootRecoveredWithFlag()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader), kRecoverKeepDeleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 2);
  CHECK(recovered[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
  CHECK(recovered[1] == NtfsBrowserTests::kOrphanedBlockOrphanName);
}

}  // namespace

TEST_CASE("TraverseSubEntries ignores an orphaned index block by default",
          "[file-record][index-block][regression]")
{
  RunOrphanedBlocksNeedRecoveryFlag<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries ignores an orphaned index block by default "
    "(FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksNeedRecoveryFlag<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds an orphaned block and rejects a "
    "stale parent",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksFoundWithRecoveryFlag<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds an orphaned block and rejects a "
    "stale parent (FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksFoundWithRecoveryFlag<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds nothing when include_deleted is "
    "off and no named record exists",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksDroppedWithoutIncludeDeleted<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds nothing when include_deleted is "
    "off and no named record exists (FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksDroppedWithoutIncludeDeleted<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan drops an entry whose named record "
    "exists but has a mismatched sequence number, when include_deleted is "
    "off",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksDroppedOnSequenceMismatch<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan drops an entry whose named record "
    "exists but has a mismatched sequence number, when include_deleted is "
    "off (FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunOrphanedBlocksDroppedOnSequenceMismatch<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries with no parsed IndexRoot reports nothing by default",
    "[file-record][index-block][regression]")
{
  RunMissingIndexRootNeedsRecoveryFlag<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries with no parsed IndexRoot reports nothing by default "
    "(FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunMissingIndexRootNeedsRecoveryFlag<Strategy::FULL_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds entries with no parsed IndexRoot "
    "at all",
    "[file-record][index-block][regression]")
{
  RunMissingIndexRootRecoveredWithFlag<Strategy::NO_CACHE>();
}

TEST_CASE(
    "TraverseSubEntries recovery scan finds entries with no parsed IndexRoot "
    "at all (FULL_CACHE)",
    "[file-record][index-block][regression]")
{
  RunMissingIndexRootRecoveredWithFlag<Strategy::FULL_CACHE>();
}
