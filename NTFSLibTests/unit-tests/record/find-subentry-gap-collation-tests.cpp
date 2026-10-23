#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
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
#include "optional-access.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// FindSubEntry() must descend into a real sub-node when the search name
// sorts past its parent entry only under NTFS' real collation order.
template <Cache::Strategy S>
void RunFindSubEntryDescendsIntoGapCollationSubNode() {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithGapCollationSubNode());

  // The named records are not built, so only recover_errors shows the entries.
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<IndexEntry> found = root.FindSubEntry(
      NTFS_BROWSER_NAMESPACE::Tests::gap_collation_search_name);
  REQUIRE(found.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(found).GetFileReference() ==
        NTFS_BROWSER_NAMESPACE::Tests::gap_collation_leaf_mft_ref);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry descends into a real sub-node across the Z-a collation gap",
    "[file-record][filename][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunFindSubEntryDescendsIntoGapCollationSubNode<S>();
}
