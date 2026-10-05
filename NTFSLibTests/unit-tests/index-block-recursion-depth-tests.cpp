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
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// FindSubEntry()/TraverseSubEntries() must not reach a leaf reachable only
// by descending past the recursion depth limit.
template <Strategy S>
void RunIndexBlockChainDepthIsBounded()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithDeepIndexBlockChain());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Mask::IndexRoot | Mask::IndexAllocation);

  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
  REQUIRE(root.ParseAttrs());

  SECTION("FindSubEntry")
  {
    const std::optional<IndexEntry> found =
        root.FindSubEntry(NtfsBrowserTests::index_block_chain_leaf_name);
    CHECK_FALSE(found.has_value());
  }

  SECTION("TraverseSubEntries")
  {
    int visited = 0;
    root.TraverseSubEntries([](const IndexEntryView&, void* context)
                            { ++(*static_cast<int*>(context)); }, &visited);
    CHECK(visited == 0);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A chained $INDEX_ALLOCATION deeper than the recursion depth limit is "
    "not fully descended",
    "[file-record][index-block][regression]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache)
{
  RunIndexBlockChainDepthIsBounded<S>();
}
