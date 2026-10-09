#include <ntfs-browser/win-types.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
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
using NtfsBrowser::IndexEntryView;
namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
namespace Mft = NtfsBrowser::Mft;

namespace {

// Both flags on: the recovery scan runs, and Decision 4's include_deleted
// filter is bypassed, so only the pre-existing parent-reference check
// applies.
constexpr VolumeOptions recover_keep_deleted{.include_deleted = true,
                                             .recover_errors = true};

// Collects every name TraverseSubEntries() reports, in callback order.
template <Cache::Strategy S>
std::vector<std::wstring> CollectNames(const FileRecord<S>& root) {
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  return names;
}

// By default, TraverseSubEntries() only follows the B+ tree's own pointers,
// so an index block no pointer reaches stays invisible.
template <Cache::Strategy S>
void RunOrphanedBlocksNeedRecoveryFlag() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> normal = CollectNames(root);
  REQUIRE(normal.size() == 1);
  // The REQUIRE above checks the size of normal.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(normal[0] == NtfsBrowserTests::orphaned_block_reachable_name);
}

// With recover_errors on and include_deleted on, every $INDEX_ALLOCATION
// block the tree walk missed is scanned too, but an entry filed under a
// different parent is rejected. include_deleted is on here because none of
// this fixture's leaf entries name a record that actually exists: with it
// off, Decision 4's filter drops every one of them instead (see
// RunOrphanedBlocksDroppedWithoutIncludeDeleted below).
template <Cache::Strategy S>
void RunOrphanedBlocksFoundWithRecoveryFlag() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 2);
  // The REQUIRE above checks the size of recovered.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(recovered[0] == NtfsBrowserTests::orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(recovered[1] == NtfsBrowserTests::orphaned_block_orphan_name);
}

// With include_deleted off (the default), Decision 4 additionally requires
// an orphan-scan entry's named record to actually exist, be in use, and
// carry a matching sequence number. Neither orphaned leaf entry (Orphan,
// Stale) names a record that exists in this fixture's tiny $MFT, so the
// orphan scan itself contributes nothing - unlike with include_deleted on
// above. Decision 4 only governs the orphan scan though: Reachable comes
// from the normal B+ tree walk and is unaffected, so it still appears.
template <Cache::Strategy S>
void RunOrphanedBlocksDroppedWithoutIncludeDeleted() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 1);
  // The REQUIRE above checks the size of recovered.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(recovered[0] == NtfsBrowserTests::orphaned_block_reachable_name);
}

// Decision 4's other drop condition: with include_deleted off, an
// orphan-scan entry naming a record that DOES exist and IS in use, but
// under a sequence number that does not match the entry's own, is dropped
// exactly like one naming a record that doesn't exist at all (see
// RunOrphanedBlocksDroppedWithoutIncludeDeleted above). With include_deleted
// on, Decision 4 is bypassed entirely and the entry is kept, confirming the
// drop above is specifically about the sequence mismatch.
template <Cache::Strategy S>
void RunOrphanedBlocksDroppedOnSequenceMismatch() {
  {
    auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
        NtfsBrowserTests::
            BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch());

    const NtfsVolume<S> volume(std::move(reader),
                               VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> recovered = CollectNames(root);
    REQUIRE(recovered.size() == 1);
    // The REQUIRE above checks the size of recovered.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(recovered[0] == NtfsBrowserTests::orphaned_block_reachable_name);
  }
  {
    auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
        NtfsBrowserTests::
            BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch());

    const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    const std::vector<std::wstring> recovered = CollectNames(root);
    REQUIRE(recovered.size() == 2);
    // The REQUIRE above checks the size of recovered.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(recovered[0] == NtfsBrowserTests::orphaned_block_reachable_name);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(recovered[1] == NtfsBrowserTests::orphaned_block_orphan_name);
  }
}

// With no $INDEX_ROOT at all, the normal walk has nowhere to start, so
// without the recovery flag TraverseSubEntries() reports nothing.
template <Cache::Strategy S>
void RunMissingIndexRootNeedsRecoveryFlag() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  CHECK(CollectNames(root).empty());
}

// With the recovery flag, a record with no parsed $INDEX_ROOT still yields
// every entry reachable through $INDEX_ALLOCATION alone.
template <Cache::Strategy S>
void RunMissingIndexRootRecoveredWithFlag() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());

  const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::vector<std::wstring> recovered = CollectNames(root);
  REQUIRE(recovered.size() == 2);
  // The REQUIRE above checks the size of recovered.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(recovered[0] == NtfsBrowserTests::orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(recovered[1] == NtfsBrowserTests::orphaned_block_orphan_name);
}

// Names TraverseSubEntries() reports on the root record of "image", with both
// recovery flags on, in callback order.
template <Cache::Strategy S>
std::vector<std::wstring> RecoverRootNames(std::vector<BYTE> image) {
  auto reader =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image));

  const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  return CollectNames(root);
}

// The scan's block count is what the mapped clusters hold, in blocks: with
// blocks smaller than a cluster, that is more than the cluster count.
template <Cache::Strategy S>
void RunSubClusterBlocksAllScanned() {
  const std::vector<std::wstring> names = RecoverRootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks());

  REQUIRE(names.size() == NtfsBrowserTests::sub_cluster_block_names.size());
  for (size_t i = 0; i < names.size(); i++) {
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[i] == NtfsBrowserTests::sub_cluster_block_names[i]);
  }
}

// The scan covers every instance of a split $INDEX_ALLOCATION, not only the
// one whose header the merged attribute keeps.
template <Cache::Strategy S>
void RunSplitAllocationAllScanned() {
  const std::vector<std::wstring> names = RecoverRootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitIndexAllocation());

  REQUIRE(names.size() == NtfsBrowserTests::split_block_names.size());
  for (size_t i = 0; i < names.size(); i++) {
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[i] == NtfsBrowserTests::split_block_names[i]);
  }
}

// Whether the entry of the fixture's VCN 2 block, filed under the parent
// generation "link" describes, is reported next to the fixture's two other
// orphan-scan entries.
template <Cache::Strategy S>
bool ParentLinkEntryReported(NtfsBrowserTests::FakeParentLink link) {
  const std::vector<std::wstring> names = RecoverRootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(
          link));

  REQUIRE(names.size() >= 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == NtfsBrowserTests::orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == NtfsBrowserTests::orphaned_block_orphan_name);
  if (names.size() == 2) {
    return false;
  }
  REQUIRE(names.size() == 3);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[2] == NtfsBrowserTests::orphaned_block_generation_name);
  return true;
}

// A stale parent sequence names an earlier directory that used the same
// record: its leftover entry is not a child of the current one.
template <Cache::Strategy S>
void RunOrphanEntryOfEarlierParentGenerationRejected() {
  CHECK_FALSE(ParentLinkEntryReported<S>({.entry_parent_sequence = 4,
                                          .record_sequence = 5,
                                          .record_in_use = true}));
  CHECK_FALSE(ParentLinkEntryReported<S>({.entry_parent_sequence = 4,
                                          .record_sequence = 6,
                                          .record_in_use = true}));
}

// The parent sequence is honoured only where it says something: the current
// generation, or 0, which claims nothing.
template <Cache::Strategy S>
void RunOrphanEntryOfCurrentParentGenerationReported() {
  CHECK(ParentLinkEntryReported<S>({.entry_parent_sequence = 5,
                                    .record_sequence = 5,
                                    .record_in_use = true}));
  CHECK(ParentLinkEntryReported<S>({.entry_parent_sequence = 0,
                                    .record_sequence = 5,
                                    .record_in_use = true}));
}

// NTFS bumps a directory's sequence number when it frees it, so a freed
// directory still owns the entries filed under its previous sequence. A live
// one does not.
template <Cache::Strategy S>
void RunOrphanEntryOfFreedParentGeneration() {
  CHECK(ParentLinkEntryReported<S>({.entry_parent_sequence = 5,
                                    .record_sequence = 6,
                                    .record_in_use = false}));
  CHECK_FALSE(ParentLinkEntryReported<S>({.entry_parent_sequence = 5,
                                          .record_sequence = 6,
                                          .record_in_use = true}));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan covers every block when index blocks "
    "are smaller than a cluster",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunSubClusterBlocksAllScanned<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan covers every instance of a split "
    "$INDEX_ALLOCATION",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunSplitAllocationAllScanned<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan rejects an entry filed under an earlier "
    "generation of the directory record",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanEntryOfEarlierParentGenerationRejected<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan reports an entry filed under the "
    "current or an unchecked generation of the directory record",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanEntryOfCurrentParentGenerationReported<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan accepts the previous generation only "
    "for a freed directory record",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanEntryOfFreedParentGeneration<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries ignores an orphaned index block by default",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedBlocksNeedRecoveryFlag<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan finds an orphaned block and rejects a "
    "stale parent",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedBlocksFoundWithRecoveryFlag<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan finds nothing when include_deleted is "
    "off and no named record exists",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedBlocksDroppedWithoutIncludeDeleted<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan drops an entry whose named record "
    "exists but has a mismatched sequence number, when include_deleted is "
    "off",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedBlocksDroppedOnSequenceMismatch<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries with no parsed IndexRoot reports nothing by default",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunMissingIndexRootNeedsRecoveryFlag<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan finds entries with no parsed IndexRoot "
    "at all",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunMissingIndexRootRecoveredWithFlag<S>();
}
