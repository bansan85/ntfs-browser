#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"
#include "name-support.h"
#include "name-tree-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNameTree;
using NTFS_BROWSER_NAMESPACE::Tests::ExoticNames;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeLink;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::FillerName;
using NTFS_BROWSER_NAMESPACE::Tests::MakeNameChain;
using NTFS_BROWSER_NAMESPACE::Tests::name_tree_root;
using NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord;
using NTFS_BROWSER_NAMESPACE::Tests::OpenNameTreeVolume;

namespace {

// Units a path may have at most for Win32: the limit that a deep chain of
// directories has to pass.
constexpr std::size_t win32_path_units = 32767;

// The path a chain of names makes: each name after a separator.
std::wstring PathOf(const std::vector<std::wstring>& names) {
  std::wstring path;
  for (const std::wstring& name : names) {
    path += L'\\';
    path += name;
  }
  return path;
}

// Scans the tree of nodes on a NoCache volume.
MftTree ScanNodes(const std::vector<FakeTreeNode>& nodes,
                  const VolumeOptions& options = {}) {
  const auto volume =
      OpenNameTreeVolume<Cache::Strategy::NoCache>(nodes, options);
  return MftTree(*volume);
}

// Restores the memo budget of the trees when it goes out of scope.
struct MemoBudget final {
  explicit MemoBudget(std::size_t units) {
    Mft::SetMaxMemoizedPathUnitsForTests(units);
    static_cast<void>(Mft::PeakMemoizedPathUnitsForTests(true));
  }

  MemoBudget(const MemoBudget&) = delete;
  MemoBudget& operator=(const MemoBudget&) = delete;
  MemoBudget(MemoBudget&&) = delete;
  MemoBudget& operator=(MemoBudget&&) = delete;

  ~MemoBudget() { Mft::SetMaxMemoizedPathUnitsForTests(std::nullopt); }
};

}  // namespace

TEST_CASE(
    "MftTree builds a path of more than 32767 units from tens of "
    "thousands of levels",
    "[mft][path][name-tree]") {
  constexpr std::size_t depth = 20000;
  const std::vector<FakeTreeNode> nodes = MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"d"); }, L"leaf.txt");
  const MftTree tree = ScanNodes(nodes);

  std::wstring expected;
  for (std::size_t level = 0; level < depth; level++) {
    expected += L"\\d";
  }
  expected += L"\\leaf.txt";
  REQUIRE(expected.size() > win32_path_units);

  std::optional<ULONGLONG> lost;
  CHECK(tree.GetPath(NameTreeRecord(depth), &lost) == expected);
  CHECK_FALSE(lost);
  CHECK(tree.IsReachable(NameTreeRecord(depth)));
  CHECK(tree.GetPath(NameTreeRecord(depth - 1)).size() == expected.size() - 9);
}

TEST_CASE("MftTree builds a path from names of 255 units at every level",
          "[mft][path][name-tree]") {
  // 140 levels of 256 units pass the 32767 of a Win32 path.
  constexpr std::size_t depth = 140;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(0x4E00 + level));
  };
  const std::vector<FakeTreeNode> nodes =
      MakeNameChain(depth, name_of, FillerName(255, L'f', L'!'));
  const MftTree tree = ScanNodes(nodes);

  std::vector<std::wstring> names;
  for (std::size_t level = 0; level < depth; level++) {
    names.push_back(name_of(level));
  }
  names.push_back(FillerName(255, L'f', L'!'));
  const std::wstring expected = PathOf(names);
  REQUIRE(expected.size() > win32_path_units);

  CHECK(tree.GetPath(NameTreeRecord(depth)) == expected);
}

TEST_CASE("MftTree keeps every kind of exotic name in a path",
          "[mft][path][name-tree]") {
  std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Exotic"}}, .directory = true}};
  for (const auto& exotic : ExoticNames()) {
    nodes.push_back({.links = {{.name = exotic.name, .parent = 0}}});
  }
  const MftTree tree = ScanNodes(nodes);

  for (std::size_t index = 0; index < ExoticNames().size(); index++) {
    INFO(ExoticNames().at(index).label);
    CHECK(tree.GetPath(NameTreeRecord(index + 1)) ==
          L"\\Exotic\\" + ExoticNames().at(index).name);
  }
}

TEST_CASE("MftTree lists names that differ only by case, each with its path",
          "[mft][path][name-tree][case]") {
  // 0-2: three files of one directory. 3, 4: directories a and A, each with
  // a file x. 7: one file with the hard links x and X.
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"readme", .name_space = 0}}},
      {.links = {{.name = L"README", .name_space = 0}}},
      {.links = {{.name = L"ReadMe", .name_space = 0}}},
      {.links = {{.name = L"a", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"A", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"x", .parent = 3}}},
      {.links = {{.name = L"x", .parent = 4}}},
      {.links = {{.name = L"x", .name_space = 0},
                 {.name = L"X", .name_space = 0}}},
  };
  const MftTree tree = ScanNodes(nodes);

  CHECK(tree.GetPath(NameTreeRecord(0)) == L"\\readme");
  CHECK(tree.GetPath(NameTreeRecord(1)) == L"\\README");
  CHECK(tree.GetPath(NameTreeRecord(2)) == L"\\ReadMe");
  CHECK(tree.GetPath(NameTreeRecord(3)) == L"\\a");
  CHECK(tree.GetPath(NameTreeRecord(4)) == L"\\A");
  CHECK(tree.GetPath(NameTreeRecord(5)) == L"\\a\\x");
  CHECK(tree.GetPath(NameTreeRecord(6)) == L"\\A\\x");
  CHECK(tree.GetPath(NameTreeRecord(7), std::size_t{0}) == L"\\x");
  CHECK(tree.GetPath(NameTreeRecord(7), std::size_t{1}) == L"\\X");

  const auto root_children = tree.Children(5);
  const std::vector<ULONGLONG> children(root_children.begin(),
                                        root_children.end());
  for (const std::size_t node : {0, 1, 2, 3, 4, 7}) {
    INFO(node);
    CHECK(std::ranges::count(children, NameTreeRecord(node)) == 1);
  }
  const auto a_children = tree.Children(NameTreeRecord(3));
  const auto big_a_children = tree.Children(NameTreeRecord(4));
  REQUIRE(a_children.size() == 1);
  REQUIRE(big_a_children.size() == 1);
  CHECK(a_children.front() == NameTreeRecord(5));
  CHECK(big_a_children.front() == NameTreeRecord(6));

  const MftTree::Entry* linked = tree.Find(NameTreeRecord(7));
  REQUIRE(linked != nullptr);
  REQUIRE(linked->names.size() == 2);
  CHECK(linked->names.at(0).name == L"x");
  CHECK(linked->names.at(1).name == L"X");
}

TEST_CASE("MftTree lists a freed Report.txt next to a live report.txt",
          "[mft][path][name-tree][case]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"report.txt"}}},
      {.links = {{.name = L"Report.txt"}}, .in_use = false},
  };

  const MftTree with_deleted = ScanNodes(nodes, {.include_deleted = true});
  CHECK(with_deleted.GetPath(NameTreeRecord(0)) == L"\\report.txt");
  CHECK(with_deleted.GetPath(NameTreeRecord(1)) == L"\\Report.txt");
  const MftTree::Entry* freed = with_deleted.Find(NameTreeRecord(1));
  REQUIRE(freed != nullptr);
  CHECK_FALSE(freed->in_use);

  const MftTree live_only = ScanNodes(nodes);
  CHECK(live_only.Find(NameTreeRecord(0)) != nullptr);
  CHECK(live_only.Find(NameTreeRecord(1)) == nullptr);
}

TEST_CASE("MftTree ends the path of a deep chain under a loop of parents",
          "[mft][path][name-tree]") {
  // Nodes 0 and 1 are each other's parent, and 2000 directories hang under 1.
  constexpr std::size_t depth = 2000;
  std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"c0", .parent = 1}}, .directory = true},
      {.links = {{.name = L"c1", .parent = 0}}, .directory = true},
  };
  for (std::size_t level = 0; level < depth; level++) {
    nodes.push_back(
        {.links = {{.name = L"d", .parent = level == 0 ? 1 : level + 1}},
         .directory = true});
  }
  const MftTree tree = ScanNodes(nodes);

  std::optional<ULONGLONG> lost;
  const std::wstring path = tree.GetPath(NameTreeRecord(depth + 1), &lost);

  REQUIRE(lost);
  CHECK(*lost == NameTreeRecord(1));
  CHECK(path.starts_with(L"c0\\c1\\d\\d"));
  CHECK(path.size() == 5 + (2 * depth));
  CHECK_FALSE(tree.IsReachable(NameTreeRecord(depth + 1)));
}

TEST_CASE("MftTree does not hold the long paths of a deep chain in its memo",
          "[mft][path][name-tree]") {
  const MemoBudget budget(std::size_t{1} << 26);
  constexpr std::size_t depth = 600;
  const std::vector<FakeTreeNode> nodes = MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"abc"); }, L"leaf");
  const MftTree tree = ScanNodes(nodes);

  std::size_t longest = 0;
  for (std::size_t level = 0; level < depth; level++) {
    const std::wstring path = tree.GetPath(NameTreeRecord(level));
    REQUIRE(path.size() == 4 * (level + 1));
    longest = std::max(longest, path.size());
  }
  CHECK(tree.GetPath(NameTreeRecord(depth)).size() == (4 * depth) + 5);
  CHECK(longest > 1024);

  // A path of more than 1024 units is not kept: the memo holds the 256
  // directories above it, 1024 units at most each.
  CHECK(Mft::PeakMemoizedPathUnitsForTests() <= 256 * 1024);
}

TEST_CASE("MftTree stops adding to its memo at the budget it is given",
          "[mft][path][name-tree]") {
  const MemoBudget budget(200);
  // 50 directories under the root, six directories in each of them: the 350
  // paths take about 2000 units.
  constexpr std::size_t parents = 50;
  constexpr std::size_t children = 6;
  std::vector<FakeTreeNode> nodes;
  for (std::size_t parent = 0; parent < parents; parent++) {
    nodes.push_back({.links = {{.name = L"p" + std::to_wstring(parent)}},
                     .directory = true});
  }
  for (std::size_t parent = 0; parent < parents; parent++) {
    for (std::size_t child = 0; child < children; child++) {
      nodes.push_back(
          {.links = {{.name = L"c" + std::to_wstring(child), .parent = parent}},
           .directory = true});
    }
  }
  const MftTree tree = ScanNodes(nodes);

  for (std::size_t parent = 0; parent < parents; parent++) {
    for (std::size_t child = 0; child < children; child++) {
      CHECK(
          tree.GetPath(NameTreeRecord(parents + (parent * children) + child)) ==
          L"\\p" + std::to_wstring(parent) + L"\\c" + std::to_wstring(child));
    }
  }
  CHECK(Mft::PeakMemoizedPathUnitsForTests() <= 200);
}

TEST_CASE(
    "MftTree gives the path of a file only the journal names, under a "
    "chain of tens of thousands of levels",
    "[mft][path][name-tree][usn]") {
  constexpr std::size_t depth = 17000;
  std::vector<BYTE> image = BuildFakeNtfsImageWithNameTree(MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"d"); }, L"leaf.txt"));
  // The journal names ghost.bin in the deepest directory, which no record
  // accounts for.
  NTFS_BROWSER_NAMESPACE::Tests::PutUsnJournal(
      image, {.records = {{.name = L"ghost.bin",
                           .file_record = NameTreeRecord(depth + 5),
                           .file_sequence = 2,
                           .parent_record = NameTreeRecord(depth - 1),
                           .parent_sequence = 1,
                           .reason = 0x80000200}}});
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  const MftTree tree(volume);

  REQUIRE(tree.JournalOnly().size() == 1);
  std::optional<ULONGLONG> lost;
  const std::wstring path = tree.GetJournalPath(0, &lost);
  CHECK_FALSE(lost);
  CHECK(path ==
        PathOf(std::vector<std::wstring>(depth, L"d")) + L"\\ghost.bin");
  CHECK(path.size() > win32_path_units);
}
