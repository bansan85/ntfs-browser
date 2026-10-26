#pragma once

#include <ntfs-browser/win-types.h>

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "mft-tree-state-support.h"

namespace NTFS_BROWSER_NAMESPACE::Tests::AccountSupport {

// First record slot a test file may take: the freed and live files of the
// 26-slot MftTree base all sit at 17 and above, and the test files replace
// them.
inline constexpr ULONGLONG first_file = 17;

// Clusters of the volume past which a run is out of range: the base image is 64
// clusters, and its bitmap clusters follow.
inline constexpr ULONGLONG far_lcn = 5000;

// A volume whose files and bitmap a test composes, then builds.
struct Volume {
  std::vector<BYTE> image = BuildFakeNtfsImageWithFreedFileTree();
  // The clusters the bitmap marks allocated.
  std::vector<FakeDataRun> allocated;
  // The clusters $BadClus lists.
  std::vector<Alloc::Extent> bad;
  // False leaves $Bitmap out: nothing is known of the allocation.
  bool with_bitmap = true;
  // How the bitmap is written, and how large the volume is.
  FakeBitmapOptions bitmap_options;

  // Writes file at slot idx.
  void Put(ULONGLONG idx, const FakeAccountFile& file) {
    PutAccountFile(image, idx, file);
  }

  // The image with its bitmap and bad list.
  [[nodiscard]] std::vector<BYTE> Build() const {
    std::vector<BYTE> built = image;
    if (with_bitmap) {
      built =
          AllocSupport::WithBitmap(std::move(built), allocated, bitmap_options);
    }
    if (!bad.empty()) {
      built = AllocSupport::WithBadClusters(std::move(built), bad);
    }
    return built;
  }
};

// A stream of the unnamed $DATA of runs, size bytes long, fully written.
inline FakeAccountStream Stream(std::vector<FakeDataRun> runs, ULONGLONG size) {
  FakeAccountStream stream;
  stream.runs = std::move(runs);
  stream.real_size = size;
  return stream;
}

// A freed file whose header carries the $LogFile sequence number lsn, with one
// stream.
inline FakeAccountFile Freed(ULONGLONG lsn, FakeAccountStream stream) {
  FakeAccountFile file;
  file.freed = true;
  file.lsn = lsn;
  file.streams.push_back(std::move(stream));
  return file;
}

// A live file with one stream.
inline FakeAccountFile Live(FakeAccountStream stream) {
  FakeAccountFile file;
  file.freed = false;
  file.streams.push_back(std::move(stream));
  return file;
}

// Options that list freed records, and account for clusters at level.
inline Mft::MftTree::ScanOptions
    Scan(Alloc::ClusterAccountLevel level = Alloc::ClusterAccountLevel::Counts,
         Alloc::DataCheckMode check = Alloc::DataCheckMode::Off) {
  Mft::MftTree::ScanOptions options;
  options.cluster_account = level;
  options.verify_data = check;
  return options;
}

// Volume options that list freed records and recover damaged ones.
inline VolumeOptions DeletedAndRecover() {
  return VolumeOptions{.include_deleted = true, .recover_errors = true};
}

// The account of record, which the test cannot go on without.
inline const Alloc::ClusterAccount& AccountOf(const Mft::MftTree& tree,
                                              ULONGLONG record) {
  static_cast<void>(MftTreeSupport::RequiredEntry(tree, record));
  const Alloc::ClusterAccount* const account = tree.ClusterAccountOf(record);
  REQUIRE(account != nullptr);
  return *account;
}

// Clusters of record counted under condition.
inline ULONGLONG CountOf(const Mft::MftTree& tree, ULONGLONG record,
                         Alloc::ClusterCondition condition) {
  return AccountOf(tree, record).tally.Count(condition);
}

// Calls body once for each cache strategy, with the strategy as a constant.
template <typename Body>
void ForEachStrategy(const Body& body) {
  body(std::integral_constant<Cache::Strategy, Cache::Strategy::NoCache>{});
  body(std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>{});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::AccountSupport
