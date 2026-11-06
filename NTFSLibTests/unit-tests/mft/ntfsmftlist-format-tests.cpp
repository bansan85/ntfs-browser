#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"
#include "mft-tree-parent-support.h"
#include "mft-tree-state-support.h"
#include "name-tree-support.h"
#include "ntfsmftlist-sample-support.h"
#include "sample-run.h"
#include "text-convert.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNameTree;
using NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::MakeNameChain;
using NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
using NTFS_BROWSER_NAMESPACE::Tests::MftListSample::RunMftList;
using NTFS_BROWSER_NAMESPACE::Tests::MftListSample::TempTreeImage;

namespace {

// Columns before the path of a line of the listing, and the length of the
// record number that opens it.
constexpr std::size_t path_column = 63;
constexpr std::size_t record_width = 10;

// Directories under the root, subdirectories in each, and files in each of
// those. A directory of the fixture takes a few dozen entries at most. Together
// they take the listing past four chunks of its threads.
constexpr std::size_t wide_tops = 10;
constexpr std::size_t wide_subs = 10;
constexpr std::size_t wide_files = 42;

// Node of the first subdirectory of top, and of the directory below the very
// first, which comes after all of them.
constexpr std::size_t FirstSub(std::size_t top) {
  return wide_tops + top * wide_subs;
}

constexpr std::size_t wide_deeper = wide_tops + wide_tops * wide_subs;

// Nodes of the wide tree that come before its files.
constexpr std::size_t wide_first_file = wide_deeper + 1;

// A tree wide enough to span many chunks, with what a listing has to carry
// through them: hard links, freed files that held clusters, a file in a deep
// directory, and a directory with two names.
std::vector<FakeTreeNode> WideTree() {
  std::vector<FakeTreeNode> nodes;
  for (std::size_t top = 0; top < wide_tops; top++) {
    nodes.push_back({.links = {{.name = L"dir" + std::to_wstring(top)}},
                     .directory = true});
  }
  for (std::size_t top = 0; top < wide_tops; top++) {
    for (std::size_t sub = 0; sub < wide_subs; sub++) {
      nodes.push_back(
          {.links = {{.name = L"sub" + std::to_wstring(sub), .parent = top}},
           .directory = true});
    }
  }
  nodes.push_back({.links = {{.name = L"deeper", .parent = FirstSub(0)}},
                   .directory = true});
  // A second name for the last of the top directories, in the first.
  nodes[wide_tops - 1].links.push_back(
      {.name = L"alias-dir", .parent = 0, .name_space = 0});
  std::size_t n = 0;
  for (std::size_t top = 0; top < wide_tops; top++) {
    for (std::size_t sub = 0; sub < wide_subs; sub++) {
      for (std::size_t i = 0; i < wide_files; i++, n++) {
        FakeTreeNode file;
        file.links.push_back(
            {.name = L"file" + std::to_wstring(n),
             .parent = n % 97 == 0 ? wide_deeper : FirstSub(top) + sub});
        if (n % 13 == 0) {
          file.links.push_back(
              {.name = L"link" + std::to_wstring(n),
               .parent = FirstSub(top) + (sub + 1) % wide_subs});
        }
        if (n % 7 == 0) {
          file.in_use = false;
          file.runs = {FakeDataRun{.lcn = 4 + n % 5, .clusters = 2}};
          file.stream_size = 2048;
        } else {
          file.data_size = static_cast<DWORD>(n % 100);
        }
        nodes.push_back(std::move(file));
      }
    }
  }
  return nodes;
}

// The wide tree, with a journal that names a file in every fifth record.
std::vector<BYTE> WideImage() {
  const std::vector<FakeTreeNode> nodes = WideTree();
  std::vector<BYTE> image = BuildFakeNtfsImageWithNameTree(nodes);
  NTFS_BROWSER_NAMESPACE::Tests::FakeUsnOptions journal;
  for (std::size_t node = wide_first_file; node < nodes.size(); node += 5) {
    journal.records.push_back(
        {.name = nodes[node].links.front().name,
         .file_record = NameTreeRecord(node),
         .file_sequence = nodes[node].in_use ? WORD{1} : WORD{2},
         .reason = node % 2 == 0
                       ? NTFS_BROWSER_NAMESPACE::Tests::fake_usn_default_reason
                       : 0x80000200});
  }
  // A file no record accounts for.
  journal.records.push_back({.name = L"ghost.bin",
                             .file_record = NameTreeRecord(nodes.size() + 40),
                             .file_sequence = 2});
  NTFS_BROWSER_NAMESPACE::Tests::PutUsnJournal(image, journal);
  return image;
}

// Runs NtfsMftList on image with options and --threads=threads, or with the
// default count when threads is 0.
SampleRun ListWith(const TempTreeImage& image,
                   const std::vector<std::wstring>& options, unsigned threads) {
  std::vector<std::wstring> args = options;
  if (threads != 0) {
    args.push_back(L"--threads=" + std::to_wstring(threads));
  }
  args.push_back(image.path.wstring());
  return RunMftList(args);
}

// How many lines text holds.
std::size_t LineCount(const std::string& text) {
  return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

}  // namespace

TEST_CASE("NtfsMftList lists a wide tree the same with every count of threads",
          "[mft][sample][threads]") {
  const TempTreeImage image(WideImage());
  const std::vector<std::wstring> options = GENERATE(
      std::vector<std::wstring>{}, std::vector<std::wstring>{L"--deleted"},
      std::vector<std::wstring>{L"--deleted", L"--recover", L"--clusters=map"});

  const SampleRun one = ListWith(image, options, 1);

  INFO("options " << options.size());
  REQUIRE(one.exit_code == 0);
  // Past three chunks of 1024 entries.
  REQUIRE(LineCount(one.out) > 3 * 1024);
  for (const unsigned threads : {2U, 3U, 4U, 8U, 0U}) {
    INFO("threads " << threads);
    const SampleRun run = ListWith(image, options, threads);

    CHECK(run.exit_code == 0);
    CHECK(run.out == one.out);
  }
}

TEST_CASE("NtfsMftList carries every kind of line through its threads",
          "[mft][sample][threads]") {
  const TempTreeImage image(WideImage());

  const SampleRun one =
      ListWith(image, {L"--deleted", L"--recover", L"--clusters=map"}, 1);
  const SampleRun four =
      ListWith(image, {L"--deleted", L"--recover", L"--clusters=map"}, 4);

  REQUIRE(one.exit_code == 0);
  CHECK(four.out == one.out);
  // The lines whose formatting a thread does: freed entries, hard links, the
  // journal, the clusters and the paths of directories of every depth.
  CHECK_THAT(one.out, ContainsSubstring(" DEL "));
  CHECK_THAT(one.out, ContainsSubstring(std::string(path_column, ' ') + "= "));
  CHECK_THAT(one.out, ContainsSubstring(std::string(path_column, ' ') + "~ "));
  CHECK_THAT(one.out,
             ContainsSubstring(std::string(path_column, ' ') + "# clusters: "));
  CHECK_THAT(one.out, ContainsSubstring(std::string(path_column, ' ') +
                                        "# stream (unnamed): 0..2048 "));
  CHECK_THAT(one.out, ContainsSubstring("\\dir0\\sub0\\deeper\\file"));
  CHECK_THAT(one.out, ContainsSubstring("Files only the journal names: 1\n"));
}

TEST_CASE("NtfsMftList lists a deep chain the same with several threads",
          "[mft][sample][threads][name]") {
  // Past the levels the listing walks itself, and its paths are not kept.
  constexpr std::size_t depth = 1500;
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(MakeNameChain(
      depth, [](std::size_t level) { return L"d" + std::to_wstring(level); },
      L"leaf")));

  const SampleRun one = ListWith(image, {}, 1);
  const SampleRun four = ListWith(image, {}, 4);

  REQUIRE(one.exit_code == 0);
  CHECK(four.out == one.out);
  CHECK(LineCount(one.out) > depth);
}

namespace {

// The text after the path column of a line of the listing that opens an entry,
// by record, and the lines that follow it, in order.
struct ListedEntry {
  std::string path;
  std::vector<std::string> other_links;
};

// The entries of a listing. A line opens an entry when a record number ends at
// the tenth column. The "=" lines that follow it are its other hard links.
std::map<ULONGLONG, ListedEntry> Parse(const std::string& listing) {
  std::map<ULONGLONG, ListedEntry> entries;
  std::istringstream lines(listing);
  std::string line;
  ULONGLONG record = 0;
  bool open = false;
  while (std::getline(lines, line)) {
    if (line.size() > path_column && line[record_width - 1] >= '0' &&
        line[record_width - 1] <= '9') {
      record = std::stoull(line.substr(0, record_width));
      entries[record].path = line.substr(path_column);
      open = true;
    } else if (open && line.size() > path_column + 2 &&
               line.compare(path_column, 2, "= ") == 0) {
      entries[record].other_links.push_back(line.substr(path_column + 2));
    }
  }
  return entries;
}

// A path as the listing shows it: after the "<lost #N>\" of a chain that
// breaks, in UTF-8, or "<no name>".
std::string Shown(const std::wstring& path,
                  const std::optional<ULONGLONG>& lost) {
  if (path.empty()) {
    return "<no name>";
  }
  return (lost ? "<lost #" + std::to_string(*lost) + ">\\" : std::string{}) +
         NtfsDevice::WideToUtf8(path);
}

// Checks that the listing of image shows, for every entry the library keeps,
// the path and the other hard links that the library gives.
void CheckPathsAgainstTheTree(const std::vector<BYTE>& image) {
  using NTFS_BROWSER_NAMESPACE::Cache::Strategy;
  const NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport::ScannedVolume<
      Strategy::NoCache>
      scanned(image);
  const TempTreeImage file(image);
  const SampleRun one = ListWith(file, {L"--deleted"}, 1);
  const SampleRun four = ListWith(file, {L"--deleted"}, 4);
  REQUIRE(one.exit_code == 0);
  CHECK(four.out == one.out);
  const std::map<ULONGLONG, ListedEntry> listed = Parse(one.out);

  CHECK(listed.size() == scanned.tree.Entries().size());
  for (const auto& entry : scanned.tree.Entries()) {
    INFO("record " << entry.record);
    const auto found = listed.find(entry.record);
    REQUIRE(found != listed.end());
    std::optional<ULONGLONG> lost;
    const std::string path =
        Shown(scanned.tree.GetPath(entry.record, &lost), lost);
    CHECK(found->second.path == path);

    std::vector<std::string> others;
    for (std::size_t i = 0; i < entry.names.size(); i++) {
      if (entry.names[i].dos_only) {
        continue;
      }
      const std::string other =
          Shown(scanned.tree.GetPath(entry.record, i, &lost), lost);
      if (other != path) {
        others.push_back(other);
      }
    }
    // A lone non-DOS name is the one the line above went through.
    if (std::ranges::count_if(entry.names, [](const auto& name) {
          return !name.dos_only;
        }) <= 1) {
      others.clear();
    }
    CHECK(found->second.other_links == others);
  }
}

}  // namespace

TEST_CASE("NtfsMftList shows the paths that the tree gives, lost ones too",
          "[mft][sample][threads][name]") {
  namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
  std::vector<BYTE> looped = BuildFakeNtfsImageWithMftTree();
  Tests::MftTreeParents::LoopDocsAndNewDir(looped);

  CheckPathsAgainstTheTree(BuildFakeNtfsImageWithMftTree());
  CheckPathsAgainstTheTree(looped);
}

TEST_CASE("NtfsMftList shows the paths that the tree gives, in a wide tree",
          "[mft][sample][threads][name]") {
  CheckPathsAgainstTheTree(BuildFakeNtfsImageWithNameTree(WideTree()));
}

TEST_CASE("NtfsMftList shows the paths that the tree gives, deep and freed",
          "[mft][sample][threads][name]") {
  // 300 levels is past the 256 the listing walks itself.
  constexpr std::size_t depth = 300;
  std::vector<FakeTreeNode> chain = MakeNameChain(
      depth, [](std::size_t level) { return L"d" + std::to_wstring(level); },
      L"leaf");
  // A freed directory in the middle of the chain, with a freed file in it, and
  // a file in a directory with two names.
  chain[150].in_use = false;
  chain.push_back({.links = {{.name = L"in-freed", .parent = 150}}});
  chain.push_back({.links = {{.name = L"two-names", .parent = 10},
                             {.name = L"other-name", .parent = 20}},
                   .directory = true});
  chain.push_back({.links = {{.name = L"inside", .parent = chain.size() - 1}}});

  CheckPathsAgainstTheTree(BuildFakeNtfsImageWithNameTree(chain));
}
