#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "memory-disk-reader.h"
#include "scratch-record.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchRecord;

namespace {

// Clusters of each run the tests encode. Four keeps the runs inside the
// 64 KiB fake volume whatever the LCN.
constexpr std::uint64_t run_clusters = 4;

// The LCN of the first run of a run list that ends at LCN 0, and the delta that
// brings it back there.
constexpr std::int64_t outward_lcn = 40;

// One run: a header byte, the length, then the LCN delta in offset_bytes bytes
// of two's complement. offset_bytes of 0 makes the run sparse.
void AppendRun(std::vector<BYTE>& runs, unsigned offset_bytes,
               std::int64_t delta) {
  runs.push_back(gsl::narrow<BYTE>((offset_bytes << 4U) | 1U));
  runs.push_back(gsl::narrow<BYTE>(run_clusters));
  const auto bits = static_cast<std::uint64_t>(delta);
  for (unsigned index = 0; index < offset_bytes; index++) {
    runs.push_back(gsl::narrow_cast<BYTE>(bits >> (8U * index)));
  }
}

// The extents of an unnamed $DATA that maps clusters virtual clusters through
// these runs, under the given recovery mode. The terminator is appended.
template <Cache::Strategy S>
Alloc::StreamExtents ExtentsOf(std::vector<BYTE> runs, ULONGLONG clusters,
                               bool recover) {
  runs.push_back(0);
  ScratchRecord scratch;
  scratch
      .NonResident(Attr::Type::Data, runs, clusters,
                   clusters * NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size)
      .End();
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(scratch.ImageIn()),
      VolumeOptions{.recover_errors = recover});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(ScratchRecord::scratch_slot));
  REQUIRE(record.ParseAttrs());
  const auto& streams = record.GetAttr(Attr::Type::Data);
  REQUIRE(streams.size() == 1);
  return streams.front()->GetExtents();
}

std::vector<std::pair<ULONGLONG, ULONGLONG>>
    AsPairs(const Alloc::StreamExtents& found) {
  std::vector<std::pair<ULONGLONG, ULONGLONG>> pairs;
  for (const Alloc::Extent& extent : found.extents) {
    pairs.emplace_back(extent.lcn, extent.clusters);
  }
  return pairs;
}

using Pairs = std::vector<std::pair<ULONGLONG, ULONGLONG>>;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A negative LCN delta in an offset field of any width is sign extended",
    "[attr-run-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const unsigned width = GENERATE(1U, 2U, 3U, 4U, 8U);
  CAPTURE(width);
  std::vector<BYTE> runs;
  AppendRun(runs, width, outward_lcn);
  AppendRun(runs, width, -outward_lcn);

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Alloc::StreamExtents found =
        ExtentsOf<S>(runs, 2 * run_clusters, recover);
    CHECK(found.complete);
    // The second run lands on LCN 0, which is a run like any other.
    CHECK(AsPairs(found) ==
          Pairs{{outward_lcn, run_clusters}, {0, run_clusters}});
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A run after a hole is relative to the last real run, not to the hole",
    "[attr-run-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> runs;
  AppendRun(runs, 1, outward_lcn);
  AppendRun(runs, 0, 0);
  AppendRun(runs, 1, -16);

  const Alloc::StreamExtents found = ExtentsOf<S>(runs, 3 * run_clusters, true);
  CHECK(found.complete);
  CHECK(AsPairs(found) ==
        Pairs{{outward_lcn, run_clusters}, {outward_lcn - 16, run_clusters}});
}

TEMPLATE_TEST_CASE_SIG("A run list of one hole has no extent",
                       "[attr-run-list][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> runs;
  AppendRun(runs, 0, 0);

  const Alloc::StreamExtents found = ExtentsOf<S>(runs, run_clusters, true);
  CHECK(found.complete);
  CHECK(found.extents.empty());
}

TEMPLATE_TEST_CASE_SIG("A first run at LCN 0 is a real run, not a hole",
                       "[attr-run-list][regression][!shouldfail]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  // A one-byte and an eight-byte offset field, both holding zero.
  const unsigned width = GENERATE(1U, 8U);
  CAPTURE(width);
  std::vector<BYTE> runs;
  AppendRun(runs, width, 0);

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Alloc::StreamExtents found =
        ExtentsOf<S>(runs, run_clusters, recover);
    CHECK(AsPairs(found) == Pairs{{0, run_clusters}});
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A run whose delta is zero repeats the LCN of the run before it",
    "[attr-run-list][regression][!shouldfail]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<BYTE> runs;
  AppendRun(runs, 1, outward_lcn);
  AppendRun(runs, 1, 0);

  const Alloc::StreamExtents found = ExtentsOf<S>(runs, 2 * run_clusters, true);
  CHECK(AsPairs(found) ==
        Pairs{{outward_lcn, run_clusters}, {outward_lcn, run_clusters}});
}
