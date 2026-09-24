#include <memory>
#include <vector>

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

// Collects every name TraverseSubEntries() reports, in callback order.
template <Strategy S>
std::vector<std::wstring> CollectNames(const FileRecord<S>& root,
                                       bool recoverOrphanedBlocks)
{
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntry& ie, void* context)
      {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            ie.GetFilename());
      },
      &names, recoverOrphanedBlocks);
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

  const std::vector<std::wstring> normal = CollectNames(root, false);
  REQUIRE(normal.size() == 1);
  CHECK(normal[0] == NtfsBrowserTests::kOrphanedBlockReachableName);
}

// With the recovery flag, every $INDEX_ALLOCATION block the tree walk missed
// is scanned too, but an entry filed under a different parent is rejected.
template <Strategy S>
void RunOrphanedBlocksFoundWithRecoveryFlag()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root, true);
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
