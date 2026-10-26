#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

using Call = Tests::CountingDiskReader::Call;
using Kind = Tests::CountingDiskReader::Kind;

// Slots of the fixture: 3000 hold about 200 files with a list in a cluster of
// its own.
constexpr ULONGLONG list_record_count = 3000;

// Clusters of one fragment of the fixture's $MFT, as the threads tests use.
constexpr ULONGLONG list_fragment_clusters = 250;

// The calls of a scan of the list fixture.
std::vector<Call> ScanCalls(const VolumeOptions& options, unsigned threads) {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          Tests::BuildFakeNtfsImageWithListMftScan(list_record_count,
                                                   list_fragment_clusters)));
  const Tests::CountingDiskReader* const counting = reader.get();
  NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  Mft::MftTree::ScanOptions scan;
  scan.threads = threads;
  const Mft::MftTree tree(volume, scan);
  return counting->Calls();
}

// Sets the restart interval for one test, and restores the default.
class IntervalOverride {
 public:
  explicit IntervalOverride(size_t records) {
    Mft::SetAnnounceRestartIntervalForTests(records);
  }

  IntervalOverride(const IntervalOverride&) = delete;
  IntervalOverride(IntervalOverride&&) = delete;
  IntervalOverride& operator=(const IntervalOverride&) = delete;
  IntervalOverride& operator=(IntervalOverride&&) = delete;

  ~IntervalOverride() { Mft::SetAnnounceRestartIntervalForTests(std::nullopt); }
};

// Whether call lies in the clusters of the non-resident lists.
bool InLists(const Call& call) {
  return call.offset >= Tests::list_scan_lists_lcn * Tests::fake_cluster_size;
}

}  // namespace

TEST_CASE("The second pass announces each list it reads before it reads it",
          "[mft][tree][prefetch]") {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const unsigned threads = GENERATE(1U, 3U);
  const std::vector<Call> calls =
      ScanCalls(VolumeOptions{.include_deleted = include_deleted,
                              .recover_errors = recover},
                threads);

  size_t lists_read = 0;
  std::vector<Call> pending;
  for (const Call& call : calls) {
    if (call.kind == Kind::Prefetch && InLists(call)) {
      pending.push_back(call);
    }
    if (call.kind != Kind::Read || !InLists(call)) {
      continue;
    }
    lists_read++;
    const auto announced =
        std::ranges::find_if(pending, [&call](const Call& hint) {
          return hint.offset == call.offset && hint.length == call.length;
        });
    INFO("read at " << call.offset << ", " << call.length << " bytes");
    CHECK(announced != pending.end());
    if (announced != pending.end()) {
      pending.erase(announced);
    }
  }
  CHECK(lists_read > 0);
}

TEST_CASE("The second pass announces the lists it reads once each",
          "[mft][tree][prefetch]") {
  const std::vector<Call> calls = ScanCalls(
      VolumeOptions{.include_deleted = true, .recover_errors = true}, 1);

  size_t hints = 0;
  size_t reads = 0;
  for (const Call& call : calls) {
    if (!InLists(call)) {
      continue;
    }
    hints += call.kind == Kind::Prefetch ? 1U : 0U;
    reads += call.kind == Kind::Read ? 1U : 0U;
  }
  CHECK(hints == reads);
}

TEST_CASE("The second pass restarts its announcements after a number of lists",
          "[mft][tree][prefetch]") {
  const VolumeOptions options{.include_deleted = true, .recover_errors = true};
  const size_t interval = GENERATE(size_t{40}, size_t{100});
  const size_t default_hints = [&] {
    size_t hints = 0;
    for (const Call& call : ScanCalls(options, 1)) {
      hints += call.kind == Kind::Prefetch && InLists(call) ? 1U : 0U;
    }
    return hints;
  }();

  const IntervalOverride restart(interval);
  size_t hints = 0;
  size_t reads = 0;
  size_t cancels = 0;
  for (const Call& call : ScanCalls(options, 1)) {
    hints += call.kind == Kind::Prefetch && InLists(call) ? 1U : 0U;
    reads += call.kind == Kind::Read && InLists(call) ? 1U : 0U;
    cancels += call.kind == Kind::CancelPrefetch ? 1U : 0U;
  }
  INFO("interval " << interval);
  CHECK(hints > default_hints);
  CHECK(cancels > 1);
  // One restart per interval of lists read, not of waiting records, and the
  // few other withdrawals of a scan.
  CHECK(cancels <= reads / interval + 4);
}
