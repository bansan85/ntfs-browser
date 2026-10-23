#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Both flags on: the recovery scan runs, and the visibility filter is
// bypassed, so only the parent-reference check applies.
constexpr VolumeOptions recover_keep_deleted{.include_deleted = true,
                                             .recover_errors = true};

// Collects every name TraverseSubEntries() reports, in callback order.
template <Cache::Strategy S>
std::vector<std::wstring>
    CollectNames(const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& root) {
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  return names;
}

// Names TraverseSubEntries() reports on the root record of image, in callback
// order. Only the attributes in mask are parsed.
template <Cache::Strategy S>
std::vector<std::wstring> RootNames(
    std::vector<BYTE> image, VolumeOptions options = recover_keep_deleted,
    Attr::Mask mask = Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation) {
  auto reader = std::make_unique<Tests::MemoryDiskReader>(std::move(image));

  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  root.SetAttrMask(mask);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  return CollectNames(root);
}

// The orphaned-blocks fixture gives the reachable entry, then the orphan.
void CheckReachableThenOrphan(const std::vector<std::wstring>& names) {
  REQUIRE(names.size() == 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == Tests::orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == Tests::orphaned_block_orphan_name);
}

// By default, TraverseSubEntries() only follows the B+ tree's own pointers,
// so an index block no pointer reaches stays invisible.
template <Cache::Strategy S>
void RunOrphanedBlocksNeedRecoveryFlag() {
  // The reachable entry names an unbuilt record, which strict mode hides.
  // The orphan block is not scanned at all without recover_errors.
  CHECK(RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedIndexBlocks(),
                     VolumeOptions{})
            .empty());
}

// With recover_errors on and include_deleted on, every $INDEX_ALLOCATION
// block the tree walk missed is scanned too, but an entry filed under a
// different parent is rejected. include_deleted is on here, so the named
// record is not checked at all (see
// RunOrphanedBlocksReportedWithoutIncludeDeleted below for the default).
template <Cache::Strategy S>
void RunOrphanedBlocksFoundWithRecoveryFlag() {
  CheckReachableThenOrphan(
      RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedIndexBlocks()));
}

// With include_deleted off (the default) and recover_errors on, the orphan
// scan reports Orphan even though its named record (102) does not parse: a
// record that cannot be read is not a freed one, so the visibility filter
// lets it through. Stale is still rejected, by its parent reference.
template <Cache::Strategy S>
void RunOrphanedBlocksReportedWithoutIncludeDeleted() {
  CheckReachableThenOrphan(
      RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedIndexBlocks(),
                   VolumeOptions{.recover_errors = true}));
}

// With include_deleted off, an orphan-scan entry naming a record that IS in
// use, but under a sequence number that does not match the entry's own, is
// hidden. A record that does not parse is not hidden (see
// RunOrphanedBlocksReportedWithoutIncludeDeleted above). With include_deleted
// on, the visibility filter is bypassed and the entry is kept, confirming the
// drop above is specifically about the sequence mismatch.
template <Cache::Strategy S>
void RunOrphanedBlocksDroppedOnSequenceMismatch() {
  const std::vector<std::wstring> strict_names = RootNames<S>(
      Tests::BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch(),
      VolumeOptions{.recover_errors = true});
  REQUIRE(strict_names.size() == 1);
  // The REQUIRE above checks the size of strict_names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(strict_names[0] == Tests::orphaned_block_reachable_name);

  CheckReachableThenOrphan(RootNames<S>(
      Tests::BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch()));
}

// With no $INDEX_ROOT at all, the normal walk has nowhere to start, so
// without the recovery flag TraverseSubEntries() reports nothing.
template <Cache::Strategy S>
void RunMissingIndexRootNeedsRecoveryFlag() {
  CHECK(RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedIndexBlocks(),
                     VolumeOptions{}, Attr::Mask::IndexAllocation)
            .empty());
}

// With the recovery flag, a record with no parsed $INDEX_ROOT still yields
// every entry reachable through $INDEX_ALLOCATION alone.
template <Cache::Strategy S>
void RunMissingIndexRootRecoveredWithFlag() {
  CheckReachableThenOrphan(
      RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedIndexBlocks(),
                   recover_keep_deleted, Attr::Mask::IndexAllocation));
}

// Same as above, but the named record is zero-filled instead of out of range.
// It still does not parse, so Orphan is reported under recover_errors alone.
template <Cache::Strategy S>
void RunOrphanedEntryZeroRecordReported() {
  CheckReachableThenOrphan(
      RootNames<S>(Tests::BuildFakeNtfsImageWithOrphanedEntryZeroRecord(),
                   VolumeOptions{.recover_errors = true}));
}

// The scan's block count is what the mapped clusters hold, in blocks: with
// blocks smaller than a cluster, that is more than the cluster count.
template <Cache::Strategy S>
void RunSubClusterBlocksAllScanned() {
  const std::vector<std::wstring> names = RootNames<S>(
      Tests::BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks());

  REQUIRE(names.size() == Tests::sub_cluster_block_names.size());
  for (size_t i = 0; i < names.size(); i++) {
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[i] == Tests::sub_cluster_block_names[i]);
  }
}

// The scan covers every instance of a split $INDEX_ALLOCATION, not only the
// one whose header the merged attribute keeps.
template <Cache::Strategy S>
void RunSplitAllocationAllScanned() {
  const std::vector<std::wstring> names =
      RootNames<S>(Tests::BuildFakeNtfsImageWithSplitIndexAllocation());

  REQUIRE(names.size() == Tests::split_block_names.size());
  for (size_t i = 0; i < names.size(); i++) {
    // The REQUIRE above checks the size of names.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(names[i] == Tests::split_block_names[i]);
  }
}

// Whether the entry of the fixture's VCN 2 block, filed under the parent
// generation "link" describes, is reported next to the fixture's two other
// orphan-scan entries.
template <Cache::Strategy S>
bool ParentLinkEntryReported(Tests::FakeParentLink link) {
  const std::vector<std::wstring> names = RootNames<S>(
      Tests::BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(link));

  REQUIRE(names.size() >= 2);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[0] == Tests::orphaned_block_reachable_name);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[1] == Tests::orphaned_block_orphan_name);
  if (names.size() == 2) {
    return false;
  }
  REQUIRE(names.size() == 3);
  // The REQUIRE above checks the size of names.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(names[2] == Tests::orphaned_block_generation_name);
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
    "TraverseSubEntries recovery scan reports an entry whose named record "
    "does not parse, when include_deleted is off",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedBlocksReportedWithoutIncludeDeleted<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scan reports an entry whose named record is "
    "zero-filled, when include_deleted is off",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunOrphanedEntryZeroRecordReported<S>();
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
