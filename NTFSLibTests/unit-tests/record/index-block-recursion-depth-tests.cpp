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

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// FindSubEntry()/TraverseSubEntries() must not reach a leaf reachable only
// by descending past the recursion depth limit.
template <Cache::Strategy S>
void RunIndexBlockChainDepthIsBounded() {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithDeepIndexBlockChain());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  SECTION("FindSubEntry") {
    const std::optional<IndexEntry> found = root.FindSubEntry(
        NTFS_BROWSER_NAMESPACE::Tests::index_block_chain_leaf_name);
    CHECK_FALSE(found.has_value());
  }

  SECTION("TraverseSubEntries") {
    int visited = 0;
    root.TraverseSubEntries(
        [](const IndexEntryView&, void* context) {
          ++(*static_cast<int*>(context));
        },
        &visited);
    CHECK(visited == 0);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A chained $INDEX_ALLOCATION deeper than the recursion depth limit is "
    "not fully descended",
    "[file-record][index-block][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunIndexBlockChainDepthIsBounded<S>();
}
