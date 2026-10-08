#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
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

#include "fake-ntfs-image.h"
#include "gap-collation-probe.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace {

// FindSubEntry() must descend into a real sub-node when the search name
// sorts past its parent entry only under NTFS' real collation order.
template <Strategy S>
void RunFindSubEntryDescendsIntoGapCollationSubNode() {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithGapCollationSubNode());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::IndexRoot | Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::gap_collation_search_name);
  REQUIRE(found.has_value());
  CHECK(NtfsBrowserTests::Unwrap(found).GetFileReference() ==
        NtfsBrowserTests::gap_collation_leaf_mft_ref);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry descends into a real sub-node across the Z-a collation gap",
    "[file-record][filename][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  RunFindSubEntryDescendsIntoGapCollationSubNode<S>();
}
