#include <cstddef>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "ntfsdir-in-process.h"
#include "win-types.h"

namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Test::Io::DirRun;
using NTFS_BROWSER_NAMESPACE::Test::Io::InImage;
using NTFS_BROWSER_NAMESPACE::Test::Io::ListInProcess;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;
using PathSet = std::set<std::string>;

namespace {

// The text of a wide string, for a message of the test.
std::string Narrow(const std::wstring& text) {
  return std::filesystem::path(text).string();
}

// The three ways to walk a volume, as the option names them.
const std::vector<std::wstring> all_engines{
    L"--engine=mftlist", L"--engine=nocache", L"--engine=fullcache"};

// The tree the tests list. Under the root: Docs (a folder) with a.txt (10
// bytes), Sub (a folder) and the deleted gone.txt; Sub holds c.txt (5 bytes)
// and Report.TXT (20 bytes); the root holds root.txt (7 bytes) and big.dat
// (600 bytes).
std::vector<BYTE> TreeImage() {
  using TestFake::FakeTreeNode;
  const auto file = [](std::wstring name, std::size_t parent, DWORD size) {
    return FakeTreeNode{.links = {{.name = std::move(name), .parent = parent}},
                        .data_size = size,
                        .index_size = size};
  };
  constexpr std::size_t docs = 0;
  constexpr std::size_t sub = 2;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Docs"}}, .directory = true});
  nodes.push_back(file(L"a.txt", docs, 10));
  nodes.push_back(
      {.links = {{.name = L"Sub", .parent = docs}}, .directory = true});
  nodes.push_back(file(L"c.txt", sub, 5));
  nodes.push_back(file(L"Report.TXT", sub, 20));
  nodes.push_back(file(L"root.txt", TestFake::name_tree_root, 7));
  nodes.push_back(file(L"big.dat", TestFake::name_tree_root, 600));
  FakeTreeNode gone = file(L"gone.txt", docs, 3);
  gone.in_use = false;
  gone.keep_entries = true;
  nodes.push_back(std::move(gone));
  return TestFake::BuildFakeNtfsImageWithNameTree(nodes);
}

// The path of each entry of listing, as "\Docs\a.txt": a path line gives it,
// and a name line takes the folder of the header above it. A listing with no
// header holds names of one folder, which count as names of the root. label is
// what the headers start with, and marked tells that a marker column comes
// before each name.
PathSet EntryPaths(const std::string& listing, const std::string& label,
                   bool marked) {
  const std::string_view header = "Directory of ";
  PathSet paths;
  std::string folder;
  std::istringstream lines(listing);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with(header)) {
      folder = line.substr(header.size() + label.size());
      continue;
    }
    if (line.empty() || line.starts_with("Files: ")) {
      continue;
    }
    std::string text = line.substr(line.rfind('\t') + 1);
    if (marked) {
      text = text.substr(2);
    }
    paths.insert(text.starts_with('\\') ? text : folder + "\\" + text);
  }
  return paths;
}

// The totals line of listing.
std::string TotalsOf(const std::string& listing) {
  const std::size_t at = listing.rfind("Files: ");
  return at == std::string::npos ? std::string() : listing.substr(at);
}

// Lists the image of a TempImage with args, which end with its target.
DirRun List(std::vector<std::wstring> args, const TempImage& image,
            std::wstring_view inside = {}) {
  args.push_back(inside.empty() ? image.path.wstring()
                                : InImage(image.path, inside));
  return ListInProcess(args);
}

}  // namespace

TEST_CASE("Every engine of NtfsDir lists the same entries of a volume",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();
  const bool deleted = GENERATE(false, true);
  INFO("deleted " << deleted);
  std::vector<std::wstring> options{L"-r"};
  if (deleted) {
    options.emplace_back(L"--deleted");
  }

  std::vector<PathSet> sets;
  std::vector<std::string> totals;
  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    std::vector<std::wstring> args = options;
    args.push_back(engine);
    const DirRun run = List(args, image);
    REQUIRE(run.exit_code == 0);
    sets.push_back(EntryPaths(run.out, label, deleted));
    totals.push_back(TotalsOf(run.out));
  }

  CHECK(sets[0].size() == (deleted ? 8 : 7));
  CHECK(sets[1] == sets[0]);
  CHECK(sets[2] == sets[0]);
  CHECK(totals[1] == totals[0]);
  CHECK(totals[2] == totals[0]);
  CHECK(sets[0].contains("\\Docs\\Sub\\c.txt"));
  CHECK(sets[0].contains("\\Docs\\gone.txt") == deleted);
}

TEST_CASE("The folder engines of NtfsDir list the children of the root",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine :
       {L"--engine=nocache", L"--engine=fullcache", L"--engine=auto"}) {
    INFO(Narrow(engine));
    const DirRun run = List({engine}, image);

    REQUIRE(run.exit_code == 0);
    CHECK(EntryPaths(run.out, label, false) ==
          PathSet{"\\Docs", "\\big.dat", "\\root.txt"});
    CHECK(TotalsOf(run.out) == "Files: 2, Directories: 1\n");
  }
}

TEST_CASE("NtfsDir --engine=mftlist lists the whole volume without -r",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const DirRun run = List({L"--engine=mftlist"}, image);

  REQUIRE(run.exit_code == 0);
  CHECK(EntryPaths(run.out, label, false).size() == 7);
  CHECK(TotalsOf(run.out) == "Files: 5, Directories: 2\n");
}

TEST_CASE("Every engine of NtfsDir lists the same entries of a folder",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  std::vector<PathSet> sets;
  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun run = List({L"-r", L"--deleted", engine}, image, L"Docs");
    REQUIRE(run.exit_code == 0);
    sets.push_back(EntryPaths(run.out, label, true));
  }

  CHECK(sets[0] == PathSet{"\\Docs\\Sub", "\\Docs\\Sub\\Report.TXT",
                           "\\Docs\\Sub\\c.txt", "\\Docs\\a.txt",
                           "\\Docs\\gone.txt"});
  CHECK(sets[1] == sets[0]);
  CHECK(sets[2] == sets[0]);
}

TEST_CASE("The two folder engines of NtfsDir write the same text",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());

  const DirRun cached =
      List({L"-r", L"--deleted", L"--engine=fullcache"}, image);
  const DirRun uncached =
      List({L"-r", L"--deleted", L"--engine=nocache"}, image);

  REQUIRE(cached.exit_code == 0);
  CHECK(uncached.exit_code == 0);
  CHECK(uncached.out == cached.out);
}

TEST_CASE("NtfsDir with Auto reads a folder by folder and a volume by path",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());

  const DirRun folder = List({L"-r"}, image, L"Docs");
  const DirRun root = List({L"-r"}, image);
  const DirRun flat = List({}, image);

  CHECK_THAT(folder.out, ContainsSubstring("\nDirectory of "));
  CHECK_THAT(folder.out, !ContainsSubstring("\t\\Docs\\a.txt\n"));
  CHECK_THAT(root.out, !ContainsSubstring("Directory of"));
  CHECK_THAT(root.out, ContainsSubstring("\t\\Docs\\a.txt\n"));
  CHECK_THAT(flat.out, !ContainsSubstring("Directory of"));
  CHECK_THAT(flat.out, !ContainsSubstring("a.txt"));
}

TEST_CASE("NtfsDir opens a block for each folder that has an entry",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const DirRun run = List({L"-r", L"--engine=fullcache"}, image);

  REQUIRE(run.exit_code == 0);
  std::vector<std::string> headers;
  std::istringstream lines(run.out);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("Directory of ")) {
      headers.push_back(line.substr(13));
    }
  }
  CHECK(headers == std::vector<std::string>{label, label + "\\Docs",
                                            label + "\\Docs\\Sub"});
  CHECK(run.out.starts_with("\nDirectory of " + label + "\n"));
}

TEST_CASE("NtfsDir marks a deleted file with --deleted in every engine",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun plain = List({engine}, image, L"Docs");
    const DirRun deleted = List({L"--deleted", engine}, image, L"Docs");

    CHECK_THAT(plain.out, !ContainsSubstring("gone.txt"));
    CHECK_THAT(plain.out, ContainsSubstring("a.txt\n"));
    CHECK_THAT(deleted.out,
               ContainsSubstring("\t* ") && ContainsSubstring("gone.txt\n"));
    CHECK_THAT(deleted.out,
               ContainsSubstring("\t  ") && ContainsSubstring("a.txt\n"));
    CHECK_THAT(deleted.out, !ContainsSubstring("\t* \\Docs\\a.txt"));
    CHECK_THAT(deleted.out, !ContainsSubstring("\t* a.txt"));
    CHECK_THAT(deleted.out, ContainsSubstring("Files: 2, Directories: 1\n"));
  }
}

TEST_CASE("NtfsDir --name keeps the names that match a wildcard",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun run = List({L"-r", L"--name=*.txt", engine}, image);

    REQUIRE(run.exit_code == 0);
    CHECK(EntryPaths(run.out, label, false) ==
          PathSet{"\\Docs\\a.txt", "\\Docs\\Sub\\Report.TXT",
                  "\\Docs\\Sub\\c.txt", "\\root.txt"});
    CHECK_THAT(run.out, ContainsSubstring("Files: 4, Directories: 0\n"));
  }
}

TEST_CASE("NtfsDir --name matches the whole name and ignores case",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const DirRun whole = List({L"-r", L"--name=a.tx"}, image);
  const DirRun folder = List({L"-r", L"--name=SUB"}, image);
  const DirRun one = List({L"-r", L"--name=?.txt"}, image);

  CHECK(EntryPaths(whole.out, label, false).empty());
  CHECK(EntryPaths(folder.out, label, false) == PathSet{"\\Docs\\Sub"});
  CHECK(EntryPaths(one.out, label, false) ==
        PathSet{"\\Docs\\Sub\\c.txt", "\\Docs\\a.txt"});
}

TEST_CASE("NtfsDir --case compares the pattern with case",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun run = List({L"-r", L"--name=*.txt", L"--case", engine}, image);

    CHECK(EntryPaths(run.out, label, false) ==
          PathSet{"\\Docs\\a.txt", "\\Docs\\Sub\\c.txt", "\\root.txt"});
  }
}

TEST_CASE("NtfsDir --match-path tests the pattern against the full path",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun run =
        List({L"-r", L"--name=\\Docs\\*.txt", L"--match-path", engine}, image);
    const DirRun by_name =
        List({L"-r", L"--name=\\Docs\\*.txt", engine}, image);

    REQUIRE(run.exit_code == 0);
    CHECK(EntryPaths(run.out, label, false) ==
          PathSet{"\\Docs\\a.txt", "\\Docs\\Sub\\Report.TXT",
                  "\\Docs\\Sub\\c.txt"});
    CHECK(EntryPaths(by_name.out, label, false).empty());
  }
}

TEST_CASE("NtfsDir --regex keeps the names a regular expression matches",
          "[io][ntfsdir][scan]") {
  if (!Lib::RegexAvailable()) {
    SKIP("this build has no regular expressions");
  }
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun run = List({L"-r", L"--regex=^[ac]\\.txt$", engine}, image);
    const DirRun path =
        List({L"-r", L"--regex=^\\\\Docs\\\\Sub\\\\", L"--match-path", engine},
             image);

    REQUIRE(run.exit_code == 0);
    CHECK(EntryPaths(run.out, label, false) ==
          PathSet{"\\Docs\\a.txt", "\\Docs\\Sub\\c.txt"});
    CHECK(EntryPaths(path.out, label, false) ==
          PathSet{"\\Docs\\Sub\\Report.TXT", "\\Docs\\Sub\\c.txt"});
  }
}

TEST_CASE("NtfsDir --regex --case tells capitals from small letters",
          "[io][ntfsdir][scan]") {
  if (!Lib::RegexAvailable()) {
    SKIP("this build has no regular expressions");
  }
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const DirRun loose = List({L"-r", L"--regex=report"}, image);
  const DirRun exact = List({L"-r", L"--regex=report", L"--case"}, image);

  CHECK(EntryPaths(loose.out, label, false) ==
        PathSet{"\\Docs\\Sub\\Report.TXT"});
  CHECK(EntryPaths(exact.out, label, false).empty());
}

TEST_CASE("NtfsDir ends with the exit code 1 for a regex that does not compile",
          "[io][ntfsdir][scan]") {
  if (!Lib::RegexAvailable()) {
    SKIP("this build has no regular expressions");
  }
  const TempImage image(TreeImage());
  (void)TestSupport::TakeCapturedLog();

  const DirRun run = List({L"--regex=("}, image);

  CHECK(run.exit_code == 1);
  CHECK(run.out.empty());
  CHECK_FALSE(TestSupport::TakeCapturedLog().empty());
}

TEST_CASE("NtfsDir --min-size and --max-size keep a range of sizes",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun big =
        List({L"-r", L"--files-only", L"--min-size=100", engine}, image);
    const DirRun tiny =
        List({L"-r", L"--files-only", L"--max-size=9", engine}, image);
    const DirRun range = List(
        {L"-r", L"--files-only", L"--min-size=10", L"--max-size=20", engine},
        image);
    const DirRun exact =
        List({L"-r", L"--files-only", L"--min-size=7", L"--max-size=7", engine},
             image);

    CHECK(EntryPaths(big.out, label, false) == PathSet{"\\big.dat"});
    CHECK(EntryPaths(tiny.out, label, false) ==
          PathSet{"\\Docs\\Sub\\c.txt", "\\root.txt"});
    CHECK(EntryPaths(range.out, label, false) ==
          PathSet{"\\Docs\\Sub\\Report.TXT", "\\Docs\\a.txt"});
    CHECK(EntryPaths(exact.out, label, false) == PathSet{"\\root.txt"});
  }
}

TEST_CASE("NtfsDir --dirs-only and --files-only split folders from files",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  for (const std::wstring& engine : all_engines) {
    INFO(Narrow(engine));
    const DirRun dirs = List({L"-r", L"--dirs-only", engine}, image);
    const DirRun files = List({L"-r", L"--files-only", engine}, image);

    CHECK(EntryPaths(dirs.out, label, false) ==
          PathSet{"\\Docs", "\\Docs\\Sub"});
    CHECK_THAT(dirs.out, ContainsSubstring("Files: 0, Directories: 2\n"));
    CHECK(EntryPaths(files.out, label, false) ==
          PathSet{"\\Docs\\Sub\\Report.TXT", "\\Docs\\Sub\\c.txt",
                  "\\Docs\\a.txt", "\\big.dat", "\\root.txt"});
    CHECK_THAT(files.out, ContainsSubstring("Files: 5, Directories: 0\n"));
  }
}

TEST_CASE("NtfsDir --system adds the system files of the volume",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const PathSet plain =
      EntryPaths(List({L"--engine=mftlist"}, image).out, label, false);
  const PathSet system = EntryPaths(
      List({L"--system", L"--engine=mftlist"}, image).out, label, false);

  CHECK_FALSE(plain.contains("\\$MFT"));
  CHECK(system.contains("\\$MFT"));
  CHECK(system.size() > plain.size());
}

TEST_CASE("NtfsDir combines a pattern and a filter with AND",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  const std::string label = image.path.string();

  const DirRun run =
      List({L"-r", L"--name=*.txt", L"--min-size=8", L"--files-only"}, image);

  CHECK(EntryPaths(run.out, label, false) ==
        PathSet{"\\Docs\\Sub\\Report.TXT", "\\Docs\\a.txt"});
}

TEST_CASE("NtfsDir ends with the exit code 1 when the scan fails",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());
  (void)TestSupport::TakeCapturedLog();

  const DirRun run = List({}, image, L"Nope");
  const std::string log = TestSupport::TakeCapturedLog();

  CHECK(run.exit_code == 1);
  CHECK(run.out.empty());
  CHECK_THAT(log, ContainsSubstring("Cannot find directory Nope"));
}

TEST_CASE("NtfsDir ends with the exit code 1 for a target that is no volume",
          "[io][ntfsdir][scan]") {
  const TempImage image(std::vector<BYTE>(4096, 0));
  (void)TestSupport::TakeCapturedLog();

  const DirRun run = List({}, image);

  CHECK(run.exit_code == 1);
  CHECK(run.out.empty());
  CHECK_THAT(TestSupport::TakeCapturedLog(),
             ContainsSubstring("Cannot get NTFS BPB"));
}

TEST_CASE("NtfsDir ends with the exit code 0 for a scan with no match",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());

  const DirRun run = List({L"-r", L"--name=nothing-like-this"}, image);

  CHECK(run.exit_code == 0);
  CHECK(run.out == "Files: 0, Directories: 0\n");
}

TEST_CASE("NtfsDir --progress writes the percent of a $MFT scan to stderr",
          "[io][ntfsdir][scan]") {
  const TempImage image(TreeImage());

  const DirRun mft = List({L"--progress", L"--engine=mftlist"}, image);
  const DirRun folder = List({L"--progress", L"--engine=fullcache"}, image);
  const DirRun silent = List({L"--engine=mftlist"}, image);

  CHECK_THAT(mft.err, ContainsSubstring("%\n"));
  CHECK(mft.err.ends_with("100%\n"));
  CHECK(folder.err.empty());
  CHECK(silent.err.empty());
  CHECK(mft.out == silent.out);
}
