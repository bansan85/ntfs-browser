#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "directory-read-ahead-support.h"
#include "probe-walk-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
namespace ProbeWalk = NTFS_BROWSER_NAMESPACE::Tests::ProbeWalk;
using Tests::FakeTreeNode;

namespace {

using Groups = std::vector<std::vector<ULONGLONG>>;
using Blocks = std::map<ULONGLONG, std::vector<BYTE>>;

// Files of the small directory of SmallTree().
constexpr std::size_t small_files = 5;

// A file node under parent.
FakeTreeNode File(std::wstring name, std::size_t parent, BYTE name_space = 1) {
  return {.links = {{.name = std::move(name),
                     .parent = parent,
                     .name_space = name_space}}};
}

// One directory that holds a few files: its index fits its record, so the
// held index is the root alone.
std::vector<FakeTreeNode> SmallTree() {
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"dir", .parent = Tests::name_tree_root}},
                   .directory = true});
  for (std::size_t file = 0; file < small_files; file++) {
    nodes.push_back(
        File(std::wstring(1, static_cast<wchar_t>(L'a' + file)), 0));
  }
  return nodes;
}

// A directory of names that are equal when upcased, and a file with two names
// that differ only by case: the same record, twice in one index.
std::vector<FakeTreeNode> TwinTree() {
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"twins",
                              .parent = Tests::name_tree_root,
                              .name_space = 0}},
                   .directory = true});
  nodes.push_back(File(L"Report.txt", 0, 0));
  nodes.push_back(File(L"report.txt", 0, 0));
  FakeTreeNode linked = File(L"Link.txt", 0, 0);
  linked.links.push_back({.name = L"LINK.TXT", .parent = 0, .name_space = 0});
  nodes.push_back(std::move(linked));
  return nodes;
}

// The groups, flattened and sorted.
std::vector<ULONGLONG> Flat(const Groups& groups) {
  std::vector<ULONGLONG> records;
  for (const std::vector<ULONGLONG>& group : groups) {
    records.insert(records.end(), group.begin(), group.end());
  }
  std::ranges::sort(records);
  return records;
}

// Whether `part` is `whole` with some groups left out, in the same order.
bool IsSubsequence(const Groups& part, const Groups& whole) {
  std::size_t next = 0;
  for (const std::vector<ULONGLONG>& group : whole) {
    if (next < part.size() && part.at(next) == group) {
      next++;
    }
  }
  return next == part.size();
}

// The held index reads nothing, logs nothing and salvages nothing.
void CheckQuiet(const ProbeWalk::DirectoryWalk& walk) {
  CHECK(walk.calls_of_groups == 0);
  CHECK(walk.log_of_groups.empty());
  CHECK(walk.recovered_by_groups == 0);
}

// The records the walk asked its store for are those of the groups, in the
// order the prober meets them.
void CheckAgainstWalk(const ProbeWalk::DirectoryWalk& walk) {
  CHECK(walk.asked == ProbeWalk::AskedBy(walk.groups));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The held index of a directory with no block is its root",
    "[io][probe-order]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(SmallTree());
  auto open = ProbeWalk::Open<S>(image);
  const Blocks blocks =
      ProbeWalk::BlocksOf(*open.volume, Tests::name_tree_first_record);
  REQUIRE(blocks.empty());

  const ProbeWalk::DirectoryWalk walk =
      ProbeWalk::WalkDirectory<S>(open, Tests::name_tree_first_record, {});
  // One node, its files in collation order: the nodes were written in it.
  REQUIRE(walk.groups.size() == 1);
  CHECK(walk.groups.front() == std::vector<ULONGLONG>{17, 18, 19, 20, 21});
  CHECK(Flat(walk.groups) == std::vector<ULONGLONG>{17, 18, 19, 20, 21});
  CheckQuiet(walk);
  CheckAgainstWalk(walk);
}

TEMPLATE_TEST_CASE_SIG(
    "The held index of a directory gives its nodes in the "
    "order the walk probes them",
    "[io][probe-order]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::BigDirectoryTree(1));
  auto open = ProbeWalk::Open<S>(image);
  const ULONGLONG directory = Tests::name_tree_first_record;
  const Blocks blocks = ProbeWalk::BlocksOf(*open.volume, directory);
  REQUIRE_FALSE(blocks.empty());

  SECTION("every block held") {
    const ProbeWalk::DirectoryWalk walk =
        ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});
    // The root, then each block.
    CHECK(walk.groups.size() > 1);
    // Each name is delivered once, and probed through one group.
    std::vector<ULONGLONG> listed = walk.listed;
    std::ranges::sort(listed);
    CHECK(Flat(walk.groups) == listed);
    CheckQuiet(walk);
    CheckAgainstWalk(walk);
  }

  SECTION("no block held") {
    const ProbeWalk::DirectoryWalk walk =
        ProbeWalk::WalkDirectory<S>(open, directory, {});
    CHECK(walk.groups.size() <= 1);
    CheckQuiet(walk);
  }
}

TEMPLATE_TEST_CASE_SIG("The held index follows the blocks of every level",
                       "[io][probe-order]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(100);
  auto open = ProbeWalk::Open<S>(image);
  const ULONGLONG directory = static_cast<ULONGLONG>(Mft::Idx::Root);
  const Blocks blocks = ProbeWalk::BlocksOf(*open.volume, directory);
  REQUIRE(blocks.size() > 2);

  const ProbeWalk::DirectoryWalk full =
      ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});
  // The root, a node of separators, and the blocks under it.
  CHECK(full.groups.size() >= 3);
  CheckQuiet(full);
  CheckAgainstWalk(full);

  const std::size_t root_only =
      ProbeWalk::WalkDirectory<S>(open, directory, {}).groups.size();
  bool dropped_inner = false;
  bool dropped_leaf = false;
  for (const auto& [vcn, bytes] : blocks) {
    Blocks without = blocks;
    without.erase(vcn);
    const ProbeWalk::DirectoryWalk walk =
        ProbeWalk::WalkDirectory<S>(open, directory, {.held = &without});
    CAPTURE(vcn);
    // A block that is not held ends the descent: what is left is the same
    // nodes, in the same order, and never more of them.
    CHECK(IsSubsequence(walk.groups, full.groups));
    CHECK(walk.groups.size() <= full.groups.size());
    CheckQuiet(walk);
    // The walk reads the missing block, and the nodes under it: its probes
    // are a superset of the held index's, in the same order.
    const Groups expected = walk.groups;
    CHECK(walk.asked.size() >= ProbeWalk::AskedBy(expected).size());
    dropped_inner = dropped_inner || walk.groups.size() == root_only;
    dropped_leaf = dropped_leaf || walk.groups.size() + 1 == full.groups.size();
  }
  // The node of separators takes everything under it with it.
  CHECK(dropped_inner);
  CHECK(dropped_leaf);
}

TEMPLATE_TEST_CASE_SIG(
    "A held block that does not parse ends its subtree "
    "without a word",
    "[io][probe-order]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::BigDirectoryTree(1));
  auto open = ProbeWalk::Open<S>(image);
  const ULONGLONG directory = Tests::name_tree_first_record;
  Blocks blocks = ProbeWalk::BlocksOf(*open.volume, directory);
  REQUIRE(blocks.size() > 2);
  const ProbeWalk::DirectoryWalk full =
      ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});

  // The magic of one block, so that its update sequence check is never reached.
  blocks.begin()->second.at(0) = 'X';
  const ProbeWalk::DirectoryWalk spoiled =
      ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});
  CHECK(spoiled.groups.size() + 1 == full.groups.size());
  CHECK(IsSubsequence(spoiled.groups, full.groups));
  CheckQuiet(spoiled);
}

TEMPLATE_TEST_CASE_SIG(
    "Entries that name one record are in the groups as many "
    "times as the walk probes it once",
    "[io][probe-order]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("aliases of the indexed scan") {
    const std::vector<BYTE> image =
        Tests::BuildFakeNtfsImageWithIndexedMftScan(100);
    auto open = ProbeWalk::Open<S>(image);
    const ULONGLONG directory = static_cast<ULONGLONG>(Mft::Idx::Root);
    const Blocks blocks = ProbeWalk::BlocksOf(*open.volume, directory);
    const ProbeWalk::DirectoryWalk walk =
        ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});

    const std::vector<ULONGLONG> flat = Flat(walk.groups);
    CHECK(std::ranges::adjacent_find(flat) != flat.end());
    // The prober asks for a record once, where the groups name it twice.
    CHECK(walk.asked.size() < flat.size());
    CheckAgainstWalk(walk);
    CheckQuiet(walk);
  }

  SECTION("names that are equal when upcased") {
    const std::vector<BYTE> image =
        Tests::BuildFakeNtfsImageWithNameTree(TwinTree());
    auto open = ProbeWalk::Open<S>(image);
    const ULONGLONG directory = Tests::name_tree_first_record;
    const ProbeWalk::DirectoryWalk walk =
        ProbeWalk::WalkDirectory<S>(open, directory, {});
    // Report.txt, report.txt, Link.txt and LINK.TXT: four entries, three
    // records.
    REQUIRE(walk.groups.size() == 1);
    const std::vector<ULONGLONG> flat = Flat(walk.groups);
    REQUIRE(flat.size() == 4);
    CHECK(flat == std::vector<ULONGLONG>{17, 18, 19, 19});
    CHECK(walk.asked == std::vector<ULONGLONG>{17, 18, 19});
    CheckAgainstWalk(walk);
    CheckQuiet(walk);
  }
}

TEMPLATE_TEST_CASE_SIG("The held index stops where the walk stops",
                       "[io][probe-order]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithDeepIndexBlockChain();
  auto open = ProbeWalk::Open<S>(image);
  const ULONGLONG directory = static_cast<ULONGLONG>(Mft::Idx::Root);
  const Blocks blocks = ProbeWalk::BlocksOf(*open.volume, directory);
  REQUIRE(blocks.size() >= Tests::index_block_chain_length);

  const ProbeWalk::DirectoryWalk walk =
      ProbeWalk::WalkDirectory<S>(open, directory, {.held = &blocks});
  // The chain is longer than the walk's depth limit, so the leaf is out of
  // reach of both.
  CHECK(walk.groups.size() < Tests::index_block_chain_length);
  CheckAgainstWalk(walk);
  CheckQuiet(walk);
}
