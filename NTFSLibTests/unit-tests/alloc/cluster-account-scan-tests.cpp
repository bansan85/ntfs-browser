#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "alloc/cluster-bitmap.h"
#include "cluster-account-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "mft-tree-state-support.h"
#include "test-log-sink.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TreeSupport = NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport;
using Alloc::ClusterCondition;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::CountOf;
using Support::Freed;
using Support::Live;
using Support::Stream;
using Tests::FakeAccountFile;
using Tests::FakeAccountStream;
using Tests::FakeDataRun;

namespace {

constexpr ULONGLONG subject = 22;
constexpr ULONGLONG live_owner = 17;
constexpr ULONGLONG extension_slot = 24;
// Cluster where the fake volumes put $MFT, which holds one record per KiB.
constexpr ULONGLONG mft_cluster = 1;
constexpr ULONGLONG cluster = Tests::fake_cluster_size;

// A $INDEX_ALLOCATION of one cluster at lcn.
FakeAccountStream IndexOn(ULONGLONG lcn) {
  FakeAccountStream stream;
  stream.type = Attr::Type::IndexAllocation;
  stream.runs = {FakeDataRun{.lcn = lcn, .clusters = 1}};
  stream.real_size = cluster;
  return stream;
}

// The subject, and a live file whose only non-resident attribute, a
// $INDEX_ALLOCATION on cluster 41, sits in an extension record.
Support::Volume WithListedIndex() {
  Support::Volume volume;
  volume.Put(subject, Freed(100, Stream({{40, 4}}, 4 * cluster)));
  FakeAccountFile owner;
  owner.freed = false;
  owner.extension_idx = extension_slot;
  owner.extension_streams = {IndexOn(41)};
  volume.Put(live_owner, owner);
  volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
  return volume;
}

// A text that holds what the account says of every entry: two scans that
// differ only in their thread count MUST give the same one.
std::string DescribeAccounts(const Mft::MftTree& tree) {
  std::string text;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    const Alloc::ClusterAccount* const account =
        tree.ClusterAccountOf(entry.record);
    if (account == nullptr) {
      continue;
    }
    text += std::format("#{}", entry.record);
    for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
      text += std::format(
          " {}", account->tally.Count(static_cast<ClusterCondition>(i)));
    }
    for (const Alloc::StreamClusterMap& map : account->streams) {
      text += std::format(" [{} {}", map.name.size(), map.data_size);
      for (const Alloc::ClusterPiece& piece : map.pieces) {
        text += std::format(" {}+{}@{}:{}", piece.first_vcn, piece.clusters,
                            piece.lcn.value_or(0),
                            static_cast<int>(piece.condition));
      }
      text += "]";
    }
    text += "\n";
  }
  const Mft::MftTree::ScanStats& stats = tree.Stats();
  text += std::format("accounts {}", stats.cluster_accounts);
  for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
    text += std::format(" {}",
                        stats.clusters.Count(static_cast<ClusterCondition>(i)));
  }
  return text;
}

}  // namespace

TEST_CASE("a non-resident attribute in an extension record is a claim",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    const TreeSupport::ScannedVolume<S> scanned(WithListedIndex().Build(),
                                                AllocSupport::DeletedOptions(),
                                                Support::Scan());

    CHECK(CountOf(scanned.tree, subject, ClusterCondition::Intact) == 3);
    CHECK(CountOf(scanned.tree, subject,
                  ClusterCondition::ReallocatedWritten) == 1);
  });
}

TEST_CASE("the account changes nothing about the tree of a healthy volume",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    const std::vector<BYTE> image = WithListedIndex().Build();
    const TreeSupport::ScannedVolume<S> off(image);
    const TreeSupport::ScannedVolume<S> on(
        image, AllocSupport::DeletedOptions(), Support::Scan());

    REQUIRE(off.tree.Entries().size() == on.tree.Entries().size());
    for (size_t i = 0; i < off.tree.Entries().size(); i++) {
      const Mft::MftTree::Entry& left = off.tree.Entries().at(i);
      const Mft::MftTree::Entry& right = on.tree.Entries().at(i);
      CHECK(left.record == right.record);
      CHECK(left.size == right.size);
      CHECK(left.allocated_size == right.allocated_size);
      CHECK(left.data_state == right.data_state);
    }
    CHECK(off.tree.Stats().damaged == on.tree.Stats().damaged);
    CHECK(off.tree.Stats().unreadable == on.tree.Stats().unreadable);
  });
}

TEST_CASE("an extension record nobody can use does not fail a record",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume = WithListedIndex();
    // The extension slot no longer holds a record.
    std::vector<BYTE> image = volume.Build();
    const size_t slot_start = (mft_cluster * cluster) +
                              (extension_slot * Tests::fake_file_record_size);
    std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(slot_start),
                Tests::fake_file_record_size, BYTE{0});
    for (const bool recover : {false, true}) {
      const TreeSupport::ScannedVolume<S> off(
          image,
          VolumeOptions{.include_deleted = true, .recover_errors = recover});
      const TreeSupport::ScannedVolume<S> on(
          image,
          VolumeOptions{.include_deleted = true, .recover_errors = recover},
          Support::Scan());

      INFO("recover " << recover);
      CHECK(off.tree.Entries().size() == on.tree.Entries().size());
      CHECK(off.tree.Stats().damaged == on.tree.Stats().damaged);
      // Nobody claims cluster 41 now: its bit is set, and nothing owns it.
      CHECK(CountOf(on.tree, subject, ClusterCondition::Intact) == 3);
      CHECK(CountOf(on.tree, subject,
                    ClusterCondition::AllocatedUnknownOwner) == 1);
    }
  });
}

TEST_CASE("a damaged attribute that is only listed does not damage a record",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume;
    FakeAccountFile file = Live(Stream({{40, 2}}, 2 * cluster));
    FakeAccountStream index = IndexOn(44);
    index.runs = {FakeDataRun{.lcn = 44, .clusters = 1},
                  FakeDataRun{.lcn = 50, .clusters = 1}};
    index.real_size = 2 * cluster;
    index.cut_after_runs = 1;
    file.streams.push_back(index);
    volume.Put(live_owner, file);
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 2}};
    const std::vector<BYTE> image = volume.Build();

    for (const bool recover : {false, true}) {
      const VolumeOptions options{.include_deleted = true,
                                  .recover_errors = recover};
      static_cast<void>(Tests::TakeCapturedLog());
      const TreeSupport::ScannedVolume<S> off(image, options);
      const std::string off_log = Tests::TakeCapturedLog();
      const TreeSupport::ScannedVolume<S> on(image, options, Support::Scan());
      const std::string on_log = Tests::TakeCapturedLog();

      INFO("recover " << recover);
      CHECK(on.tree.Stats().damaged == off.tree.Stats().damaged);
      CHECK(on.tree.Find(live_owner) != nullptr);
      CHECK(on.tree.ClusterAccountOf(live_owner) == nullptr);
      // Nothing about the attribute reaches the log: no decode error.
      CHECK(on_log.find("DataRun decode error") ==
            off_log.find("DataRun decode error"));
    }
  });
}

TEST_CASE("the account is the same whatever the thread count",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume = WithListedIndex();
    volume.Put(23, Freed(200, Stream({{42, 2}}, 2 * cluster)));
    FakeAccountFile damaged = Live(Stream({{44, 2}, {50, 2}}, 4 * cluster));
    damaged.streams.front().cut_after_runs = 2;
    volume.Put(18, damaged);
    const std::vector<BYTE> image = volume.Build();

    std::string expected;
    std::vector<Tests::CountingDiskReader::Call> expected_calls;
    for (const unsigned threads : {1U, 2U, 3U, 8U}) {
      Mft::MftTree::ScanOptions options =
          Support::Scan(Alloc::ClusterAccountLevel::CountsAndMap);
      options.threads = threads;
      const TreeSupport::ScannedVolume<S> scanned(
          image, Support::DeletedAndRecover(), options);

      const std::string text = DescribeAccounts(scanned.tree);
      INFO("threads " << threads);
      INFO(text);
      CHECK(text.find("#22") != std::string::npos);
      CHECK(text.find("#18") != std::string::npos);
      const auto& calls = scanned.opened.counter->Calls();
      if (threads == 1) {
        expected = text;
        expected_calls = calls;
        continue;
      }
      CHECK(text == expected);
      CHECK(std::ranges::equal(
          calls, expected_calls, [](const auto& left, const auto& right) {
            return left.kind == right.kind && left.offset == right.offset &&
                   left.length == right.length;
          }));
    }
  });
}

TEST_CASE("the account reads $Bitmap once, in ascending order",
          "[alloc][cluster][scan]") {
  // Windows of one cluster of bitmap each, which cover 8192 clusters.
  Alloc::SetBitmapWindowBytesForTests(Tests::fake_cluster_size);
  constexpr ULONGLONG volume_clusters = 20000;
  Support::Volume volume;
  volume.bitmap_options.volume_clusters = volume_clusters;
  // Freed files in the first three windows.
  volume.Put(subject, Freed(100, Stream({{40, 4}}, 4 * cluster)));
  volume.Put(23, Freed(200, Stream({{9000, 2}}, 2 * cluster)));
  volume.Put(24, Freed(300, Stream({{17000, 2}}, 2 * cluster)));
  const std::vector<BYTE> image = volume.Build();

  for (const bool annotate : {true, false}) {
    Mft::MftTree::ScanOptions options = Support::Scan();
    options.annotate_data_state = annotate;
    const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> scanned(
        image, AllocSupport::DeletedOptions(), options);

    INFO("annotate " << annotate);
    std::vector<ULONGLONG> offsets;
    for (const Tests::CountingDiskReader::Call& call :
         scanned.opened.counter->Calls()) {
      if (call.kind == Tests::CountingDiskReader::Kind::Read &&
          call.offset >= volume_clusters * cluster) {
        offsets.push_back(call.offset);
      }
    }
    CHECK(offsets.size() >= 3);
    // No window twice, and none out of order: the ascending sweep.
    CHECK(std::ranges::is_sorted(offsets));
    CHECK(std::ranges::adjacent_find(offsets) == offsets.end());
    CHECK(CountOf(scanned.tree, 24, ClusterCondition::Intact) == 2);
  }
  Alloc::SetBitmapWindowBytesForTests(0);
}

TEST_CASE("the account never touches a volume with nothing to keep",
          "[alloc][cluster][scan]") {
  const std::vector<BYTE> image = WithListedIndex().Build();
  const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> plain(
      image, VolumeOptions{});
  const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> asked(
      image, VolumeOptions{}, Support::Scan());

  CHECK(plain.opened.counter->ReadCount() == asked.opened.counter->ReadCount());
}

TEST_CASE("cluster runs follow the bitmap and the bad list",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume;
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 4}};
    volume.bad = {Alloc::Extent{.lcn = 42, .clusters = 1}};
    const auto opened = AllocSupport::OpenVolume<S>(volume.Build());
    std::vector<Alloc::ClusterRun> runs;

    opened.volume->GetClusterRuns(38, 10, runs);

    REQUIRE(runs.size() == 5);
    CHECK(runs.at(0).state == Alloc::ClusterState::Free);
    CHECK(runs.at(0).lcn == 38);
    CHECK(runs.at(0).clusters == 2);
    CHECK(runs.at(1).state == Alloc::ClusterState::Allocated);
    CHECK(runs.at(1).clusters == 2);
    CHECK(runs.at(2).state == Alloc::ClusterState::Bad);
    CHECK(runs.at(2).lcn == 42);
    CHECK(runs.at(2).clusters == 1);
    CHECK(runs.at(3).state == Alloc::ClusterState::Allocated);
    CHECK(runs.at(4).state == Alloc::ClusterState::Free);
    CHECK(runs.at(4).lcn == 44);
    CHECK(runs.at(4).clusters == 4);

    opened.volume->GetClusterRuns(Support::far_lcn, 3, runs);
    REQUIRE(runs.size() == 1);
    CHECK(runs.at(0).state == Alloc::ClusterState::OutOfRange);
    CHECK(runs.at(0).clusters == 3);

    opened.volume->GetClusterRuns(40, 0, runs);
    CHECK(runs.empty());
  });
}

TEST_CASE("cluster runs are unknown when $Bitmap is lost",
          "[alloc][cluster][scan]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume;
    volume.with_bitmap = false;
    const auto opened = AllocSupport::OpenVolume<S>(volume.Build());
    std::vector<Alloc::ClusterRun> runs;

    opened.volume->GetClusterRuns(40, 6, runs);

    REQUIRE(runs.size() == 1);
    CHECK(runs.at(0).state == Alloc::ClusterState::Unknown);
    CHECK(runs.at(0).lcn == 40);
    CHECK(runs.at(0).clusters == 6);
  });
}
