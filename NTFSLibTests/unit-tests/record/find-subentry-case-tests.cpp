#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "name-support.h"
#include "name-tree-support.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::CountingDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::FakeNameTreeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::FillerName;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord;
using NTFS_BROWSER_NAMESPACE::Tests::OpenNameTreeVolume;

namespace {

// The three spellings of one name that the tests keep in one directory, in the
// order the index keeps them: equal when upcased, so by code unit.
const std::vector<std::wstring> twin_names{L"README", L"ReadMe", L"readme"};

// A file node called name, a POSIX name: NTFS lets two of them differ only by
// case.
FakeTreeNode PosixFile(std::wstring name) {
  return {.links = {{.name = std::move(name), .name_space = 0}}};
}

// The three twins as nodes 0 to 2.
std::vector<FakeTreeNode> TwinNodes() {
  std::vector<FakeTreeNode> nodes;
  for (const std::wstring& name : twin_names) {
    nodes.push_back(PosixFile(name));
  }
  return nodes;
}

// Looks name up in the root of a volume.
template <Cache::Strategy S>
std::optional<IndexEntry> FindInRoot(const NtfsVolume<S>& volume,
                                     std::wstring_view name) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  return root.FindSubEntry(name);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("FindSubEntry takes the entry spelled as asked",
                       "[file-record][case][name-tree]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const std::vector<FakeTreeNode> nodes = TwinNodes();
  const auto volume = OpenNameTreeVolume<S>(nodes);

  for (std::size_t index = 0; index < twin_names.size(); index++) {
    INFO(index);
    const std::optional<IndexEntry> found =
        FindInRoot(*volume, twin_names.at(index));
    REQUIRE(found);
    CHECK(found->GetFilename() == twin_names.at(index));
    CHECK(found->GetFileReference() == NameTreeRecord(index));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry takes the first equal entry when no spelling is exact",
    "[file-record][case][name-tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<FakeTreeNode> nodes = TwinNodes();
  const auto volume = OpenNameTreeVolume<S>(nodes);

  // The index holds README, ReadMe, readme: the first stands in.
  const std::optional<IndexEntry> found = FindInRoot(*volume, L"rEADME");

  REQUIRE(found);
  CHECK(found->GetFilename() == L"README");
  CHECK(found->GetFileReference() == NameTreeRecord(0));
}

TEMPLATE_TEST_CASE_SIG("FindSubEntry finds no name that is not there",
                       "[file-record][case][name-tree]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const std::vector<FakeTreeNode> nodes = TwinNodes();
  const auto volume = OpenNameTreeVolume<S>(nodes);

  CHECK_FALSE(FindInRoot(*volume, L"readmee"));
  CHECK_FALSE(FindInRoot(*volume, L"readm"));
  CHECK_FALSE(FindInRoot(*volume, L"zzz"));
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry takes the exact spelling wherever the index blocks put it",
    "[file-record][case][name-tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Names of 20 units make about seven entries a block: the fillers that sort
  // before the twins move them from block to block, and the separators land on
  // each of them in turn.
  for (std::size_t fillers = 0; fillers < 36; fillers++) {
    INFO(fillers);
    std::vector<FakeTreeNode> nodes = TwinNodes();
    for (std::size_t index = 0; index < fillers; index++) {
      nodes.push_back(PosixFile(
          FillerName(20, L'a', static_cast<wchar_t>(L'a' + index % 26)) +
          std::to_wstring(index)));
    }
    const auto volume = OpenNameTreeVolume<S>(nodes);

    for (std::size_t index = 0; index < twin_names.size(); index++) {
      INFO(twin_names.at(index).size());
      const std::optional<IndexEntry> found =
          FindInRoot(*volume, twin_names.at(index));
      REQUIRE(found);
      CHECK(found->GetFilename() == twin_names.at(index));
      CHECK(found->GetFileReference() == NameTreeRecord(index));
    }
    const std::optional<IndexEntry> unspelled = FindInRoot(*volume, L"rEaDmE");
    REQUIRE(unspelled);
    CHECK(unspelled->GetFileReference() == NameTreeRecord(0));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry puts the twins of a directory under each directory apart",
    "[file-record][case][name-tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Directories a and A, each with a child x: a walk down a/x and A/x ends in
  // two different records.
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"a", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"A", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"x", .parent = 0}}},
      {.links = {{.name = L"x", .parent = 1}}},
  };
  const auto volume = OpenNameTreeVolume<S>(nodes);

  for (const std::size_t directory : {std::size_t{0}, std::size_t{1}}) {
    const std::wstring name = directory == 0 ? L"a" : L"A";
    INFO(directory);
    const std::optional<IndexEntry> found = FindInRoot(*volume, name);
    REQUIRE(found);
    CHECK(found->GetFileReference() == NameTreeRecord(directory));

    NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(*volume);
    record.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    REQUIRE(record.ParseFileRecord(found->GetFileReference()));
    REQUIRE(record.ParseAttrs());
    const std::optional<IndexEntry> child = record.FindSubEntry(L"x");
    REQUIRE(child);
    CHECK(child->GetFileReference() == NameTreeRecord(directory + 2));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry takes the live twin when the exact spelling is freed",
    "[file-record][case][name-tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The index still names the freed Report.txt, which sorts before report.txt.
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Report.txt", .name_space = 0}},
       .in_use = false,
       .keep_entries = true},
      PosixFile(L"report.txt"),
  };
  const auto volume = OpenNameTreeVolume<S>(nodes);

  SECTION("a volume that hides deleted items") {
    const std::optional<IndexEntry> exact = FindInRoot(*volume, L"Report.txt");
    REQUIRE(exact);
    CHECK(exact->GetFilename() == L"report.txt");
    CHECK(exact->GetFileReference() == NameTreeRecord(1));
  }

  SECTION("a volume that lists deleted items") {
    const auto listing =
        OpenNameTreeVolume<S>(nodes, VolumeOptions{.include_deleted = true});

    const std::optional<IndexEntry> freed = FindInRoot(*listing, L"Report.txt");
    REQUIRE(freed);
    CHECK(freed->GetFilename() == L"Report.txt");
    CHECK(freed->GetFileReference() == NameTreeRecord(0));
    const std::optional<IndexEntry> live = FindInRoot(*listing, L"report.txt");
    REQUIRE(live);
    CHECK(live->GetFileReference() == NameTreeRecord(1));
  }
}

TEST_CASE("FindSubEntry stops past the twins of a freed name it hides",
          "[file-record][case][name-tree]") {
  constexpr auto no_cache = Cache::Strategy::NoCache;
  // The freed Report.txt, and fillers after it that fill several index blocks.
  std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Report.txt"}},
       .in_use = false,
       .keep_entries = true},
  };
  for (std::size_t index = 0; index < 30; index++) {
    nodes.push_back({.links = {{.name = FillerName(20, L's',
                                                   static_cast<wchar_t>(
                                                       L'a' + index % 26)) +
                                        std::to_wstring(index)}}});
  }
  auto counter =
      std::make_unique<CountingDiskReader>(std::make_unique<MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNameTree(
              nodes)));
  CountingDiskReader* reads = counter.get();
  NtfsVolume<no_cache> volume(std::move(counter));
  REQUIRE(volume.IsVolumeOK());

  reads->Reset();
  CHECK_FALSE(FindInRoot(volume, L"Report.txu"));
  const std::size_t absent_reads = reads->ReadCount();
  reads->Reset();
  CHECK_FALSE(FindInRoot(volume, L"Report.txt"));
  const std::size_t hidden_reads = reads->ReadCount();

  // Both end in the scan of every entry, since this volume has no $UpCase. A
  // name that is not there reads every block after its place first. The hidden
  // name reads its own record, then one block more at most, where a twin of
  // another spelling could sit: it costs less.
  CAPTURE(absent_reads, hidden_reads);
  CHECK(hidden_reads < absent_reads);
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry scans every entry for the exact spelling when the order "
    "cannot be trusted",
    "[file-record][case][name-tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The index is not in collation order: a search for "readme" meets "zzz"
  // first and gives up. The scan that follows takes the exact spelling, not the
  // first equal entry.
  const std::vector<FakeTreeNode> nodes{
      PosixFile(L"zzz"),
      PosixFile(L"README"),
      PosixFile(L"readme"),
  };
  const auto volume =
      OpenNameTreeVolume<S>(nodes, {}, FakeNameTreeOptions{.collate = false});

  const std::optional<IndexEntry> exact = FindInRoot(*volume, L"readme");
  REQUIRE(exact);
  CHECK(exact->GetFilename() == L"readme");
  CHECK(exact->GetFileReference() == NameTreeRecord(2));

  const std::optional<IndexEntry> upper = FindInRoot(*volume, L"README");
  REQUIRE(upper);
  CHECK(upper->GetFileReference() == NameTreeRecord(1));

  const std::optional<IndexEntry> other = FindInRoot(*volume, L"ReadMe");
  REQUIRE(other);
  CHECK(other->GetFileReference() == NameTreeRecord(1));
}

TEMPLATE_TEST_CASE_SIG("FindSubEntry finds every kind of exotic name",
                       "[file-record][name-tree]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  std::vector<FakeTreeNode> nodes;
  for (const auto& exotic : NTFS_BROWSER_NAMESPACE::Tests::ExoticNames()) {
    nodes.push_back({.links = {{.name = exotic.name}}});
  }
  const auto volume = OpenNameTreeVolume<S>(nodes);

  for (std::size_t index = 0; index < nodes.size(); index++) {
    const auto& exotic = NTFS_BROWSER_NAMESPACE::Tests::ExoticNames().at(index);
    INFO(exotic.label);
    const std::optional<IndexEntry> found = FindInRoot(*volume, exotic.name);
    REQUIRE(found);
    CHECK(found->GetFilename() == exotic.name);
    CHECK(found->GetFileReference() == NameTreeRecord(index));
  }
}

TEMPLATE_TEST_CASE_SIG("FindSubEntry finds the DOS alias beside a long name",
                       "[file-record][name-tree]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"LongName.txt"},
                 {.name = L"LONGNA~1.TXT", .name_space = 2}}}};
  const auto volume = OpenNameTreeVolume<S>(nodes);

  const std::optional<IndexEntry> long_name =
      FindInRoot(*volume, L"LongName.txt");
  const std::optional<IndexEntry> alias = FindInRoot(*volume, L"LONGNA~1.TXT");

  REQUIRE(long_name);
  REQUIRE(alias);
  CHECK(long_name->IsWin32Name());
  CHECK_FALSE(alias->IsWin32Name());
  CHECK(long_name->GetFileReference() == NameTreeRecord(0));
  CHECK(alias->GetFileReference() == NameTreeRecord(0));
}
