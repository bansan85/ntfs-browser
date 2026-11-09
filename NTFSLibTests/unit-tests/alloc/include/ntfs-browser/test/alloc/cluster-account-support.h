#pragma once

#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/fake-image/account-volume.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/mft/mft-tree-state-support.h>

#include "alloc/cluster-account.h"
#include "alloc/types.h"
#include "cache/strategy.h"
#include "mft/tree.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Alloc {

// First record slot a test file may take: the freed and live files of the
// 26-slot MftTree base all sit at 17 and above, and the test files replace
// them.
inline constexpr ULONGLONG first_file = 17;

// Clusters of the volume past which a run is out of range: the base image is 64
// clusters, and its bitmap clusters follow.
inline constexpr ULONGLONG far_lcn = Test::FakeImage::account_far_lcn;

// A volume whose files and bitmap a test composes, then builds.
using Volume = Test::FakeImage::FakeAccountVolume;

// A stream of the unnamed $DATA of runs, size bytes long, fully written.
inline Test::FakeImage::FakeAccountStream
    Stream(std::vector<Test::FakeImage::FakeDataRun> runs, ULONGLONG size) {
  return Test::FakeImage::MakeAccountStream(std::move(runs), size);
}

// A freed file whose header carries the $LogFile sequence number lsn, with one
// stream.
inline Test::FakeImage::FakeAccountFile
    Freed(ULONGLONG lsn, Test::FakeImage::FakeAccountStream stream) {
  return Test::FakeImage::MakeFreedAccountFile(lsn, std::move(stream));
}

// A live file with one stream.
inline Test::FakeImage::FakeAccountFile
    Live(Test::FakeImage::FakeAccountStream stream) {
  return Test::FakeImage::MakeLiveAccountFile(std::move(stream));
}

// Options that list freed records, and account for clusters at level.
inline NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions
    Scan(NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel level =
             NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel::Counts,
         NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode check =
             NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode::Off) {
  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions options;
  options.cluster_account = level;
  options.verify_data = check;
  return options;
}

// Volume options that list freed records and recover damaged ones.
inline VolumeOptions DeletedAndRecover() {
  return VolumeOptions{.include_deleted = true, .recover_errors = true};
}

// The account of record, which the test cannot go on without.
inline const NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccount&
    AccountOf(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
              ULONGLONG record) {
  static_cast<void>(
      NTFS_BROWSER_NAMESPACE::Test::Mft::RequiredEntry(tree, record));
  const NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccount* const account =
      tree.ClusterAccountOf(record);
  REQUIRE(account != nullptr);
  return *account;
}

// Clusters of record counted under condition.
inline ULONGLONG
    CountOf(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree, ULONGLONG record,
            NTFS_BROWSER_NAMESPACE::Alloc::ClusterCondition condition) {
  return AccountOf(tree, record).tally.Count(condition);
}

// Calls body once for each cache strategy, with the strategy as a constant.
template <typename Body>
void ForEachStrategy(const Body& body) {
  body(std::integral_constant<
       NTFS_BROWSER_NAMESPACE::Cache::Strategy,
       NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>{});
  body(std::integral_constant<
       NTFS_BROWSER_NAMESPACE::Cache::Strategy,
       NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>{});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Alloc
