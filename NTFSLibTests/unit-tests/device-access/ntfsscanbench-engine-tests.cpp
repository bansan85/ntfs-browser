#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/mft/ntfsscanbench-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>

namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestMft = NTFS_BROWSER_NAMESPACE::Test::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Test::Io::DirectoryHeader;
using NTFS_BROWSER_NAMESPACE::Test::Io::DocsListing;
using NTFS_BROWSER_NAMESPACE::Test::Io::InImage;
using NTFS_BROWSER_NAMESPACE::Test::Io::ListingLine;
using NTFS_BROWSER_NAMESPACE::Test::Io::RootListing;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;
using TestSupport::sample_failure_exit_code;
using TestSupport::SampleRun;
namespace Fs = std::filesystem;

namespace {

// The two engines that walk the directories, as the option writes them.
const std::vector<std::wstring> walk_engines{L"--engine=nocache",
                                             L"--engine=fullcache"};

// The text of a wide string, for a message of the test.
std::string Narrow(const std::wstring& text) { return Fs::path(text).string(); }

// directory with name below it, as a walk writes a path: with the separator of
// the host.
std::string Below(const std::string& directory, std::string_view name) {
  return directory + static_cast<char>(Fs::path::preferred_separator) +
         std::string(name);
}

// Runs the tool with the engine option first, then args.
SampleRun RunEngine(const std::wstring& engine,
                    std::vector<std::wstring> args) {
  args.insert(args.begin(), engine);
  return TestMft::RunScanBench(args);
}

// A tree that the three engines list: Docs with a.txt and Sub, Sub with c.txt,
// and root.txt in the root.
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
  nodes.push_back(file(L"root.txt", TestFake::name_tree_root, 7));
  return TestFake::BuildFakeNtfsImageWithNameTree(nodes);
}

// The rows of a comparison table: the cells of each line after its header.
std::vector<std::vector<std::string>> TableRows(const std::string& table) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream lines(table);
  std::string line;
  // The messages of an engine that failed come before the table.
  while (std::getline(lines, line) && !line.starts_with("Engine")) {
  }
  while (std::getline(lines, line)) {
    std::istringstream cells(line);
    std::vector<std::string> row;
    for (std::string cell; cells >> cell;) {
      row.push_back(cell);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

TEST_CASE("NtfsScanBench walks the root with the nocache and fullcache engines",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  for (const std::wstring& engine : walk_engines) {
    INFO(Narrow(engine));
    const SampleRun run = TestMft::RunScanBench({engine, image.path.wstring()});

    CHECK(run.exit_code == 0);
    CHECK(run.out == RootListing());
  }
}

TEST_CASE("NtfsScanBench walks a directory of an image with a walk engine",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  for (const std::wstring& engine : walk_engines) {
    INFO(Narrow(engine));
    const std::wstring target = InImage(image.path, L"Docs");

    const SampleRun plain = TestMft::RunScanBench({engine, target});
    CHECK(plain.exit_code == 0);
    CHECK(plain.out == DocsListing("report.txt"));

    const SampleRun deleted =
        TestMft::RunScanBench({engine, L"--deleted", target});
    CHECK(deleted.exit_code == 0);
    CHECK(deleted.out == DocsListing("report.txt", " "));

    const SampleRun recovered =
        TestMft::RunScanBench({engine, L"--recover", target});
    CHECK(recovered.exit_code == 0);
    CHECK(recovered.out == DocsListing("report.txt"));
  }
}

TEST_CASE("NtfsScanBench lists every directory under a header with --recursive",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string expected =
      "\nDirectory of " + root + "\n" + ListingLine("Docs", true, 0) +
      "\nDirectory of " + docs + "\n" +
      ListingLine("report.txt", false, TestFake::dump_broken_alter_time) +
      "Files: 1, Directories: 1\n";

  for (const std::wstring& engine : walk_engines) {
    for (const std::wstring option : {L"--recursive", L"-r"}) {
      INFO(Narrow(engine) << " " << Narrow(option));
      const SampleRun run =
          TestMft::RunScanBench({engine, option, image.path.wstring()});

      CHECK(run.exit_code == 0);
      CHECK(run.out == expected);
    }
  }
}

TEST_CASE("NtfsScanBench walks skip a directory that an entry loops back to",
          "[device-access][scanbench][sample]") {
  const TempImage image(
      TestFake::BuildFakeNtfsImageWithOrphanedDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const std::string again = Below(sub, "Docs");

  for (const std::wstring& engine : walk_engines) {
    INFO(Narrow(engine));
    const SampleRun run = TestMft::RunScanBench(
        {engine, L"--recursive", L"--recover", image.path.wstring()});

    CHECK(run.exit_code == 0);
    CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                         DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                         DirectoryHeader(sub) + ListingLine("Docs", true, 0) +
                         "Skipping " + again + ": directory already listed\n" +
                         "Files: 0, Directories: 3\n");
  }
}

TEST_CASE("NtfsScanBench walks say why a directory cannot be listed",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  for (const std::wstring& engine : walk_engines) {
    INFO(Narrow(engine));
    const SampleRun missing =
        TestMft::RunScanBench({engine, InImage(image.path, L"Nope")});
    CHECK(missing.exit_code == sample_failure_exit_code);
    CHECK(missing.out == "Cannot find directory Nope\n");

    const SampleRun file = TestMft::RunScanBench(
        {engine, InImage(image.path, L"Docs/report.txt")});
    CHECK(file.exit_code == sample_failure_exit_code);
    CHECK(file.out == "report.txt is not a directory\n");
  }
}

TEST_CASE("NtfsScanBench walks report an image that is not an NTFS volume",
          "[device-access][scanbench][sample]") {
  const TempImage image(std::vector<BYTE>(4096, 0));
  for (const std::wstring& engine : walk_engines) {
    INFO(Narrow(engine));
    const SampleRun run = TestMft::RunScanBench(
        {engine, L"--log=console:off", image.path.wstring()});

    CHECK(run.exit_code == sample_failure_exit_code);
    CHECK_THAT(run.out, ContainsSubstring("Cannot get NTFS BPB"));
  }
}

TEST_CASE("NtfsScanBench lists with the mftlist engine by default",
          "[device-access][scanbench][sample]") {
  const TestMft::TempTreeImage image;
  const SampleRun plain = TestMft::RunScanBench({image.path.wstring()});
  const SampleRun named =
      RunEngine(L"--engine=mftlist", {image.path.wstring()});

  REQUIRE(plain.exit_code == 0);
  CHECK(named.exit_code == 0);
  CHECK(named.out == plain.out);
  CHECK(named.err == plain.err);
  CHECK_THAT(plain.out, ContainsSubstring("Record"));
  CHECK_THAT(plain.out, ContainsSubstring("Record slots: "));
}

TEST_CASE("NtfsScanBench refuses an engine it does not know",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  for (const std::wstring engine : {L"--engine=", L"--engine=fast",
                                    L"--engine=MFTLIST", L"--engine=all,all"}) {
    INFO(Narrow(engine));
    const SampleRun run = TestMft::RunScanBench({engine, image.path.wstring()});

    CHECK(run.exit_code == sample_failure_exit_code);
    CHECK_THAT(run.err, ContainsSubstring("Unknown engine"));
    CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsscanbench"));
  }
}

TEST_CASE("NtfsScanBench usage names the engines and the quiet option",
          "[device-access][scanbench][sample]") {
  const SampleRun run = TestMft::RunScanBench({});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK_THAT(run.out,
             ContainsSubstring("--engine=mftlist|nocache|fullcache|auto|all"));
  CHECK_THAT(run.out, ContainsSubstring("--quiet"));
  CHECK_THAT(run.out, ContainsSubstring("--recursive"));
}

TEST_CASE("NtfsScanBench --quiet writes no listing",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  const TestMft::TempTreeImage tree;
  for (const std::wstring engine :
       {L"--engine=nocache", L"--engine=fullcache", L"--engine=mftlist"}) {
    INFO(Narrow(engine));
    const Fs::path& path =
        engine == L"--engine=mftlist" ? tree.path : image.path;
    const SampleRun run =
        TestMft::RunScanBench({engine, L"--quiet", path.wstring()});

    CHECK(run.exit_code == 0);
    CHECK(run.out.empty());
  }
}

TEST_CASE("NtfsScanBench --quiet still says why a walk fails",
          "[device-access][scanbench][sample]") {
  const TempImage image;
  const SampleRun run = TestMft::RunScanBench(
      {L"--engine=nocache", L"--quiet", InImage(image.path, L"Nope")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "Cannot find directory Nope\n");
}

TEST_CASE("NtfsScanBench --engine=all prints a table of three engines",
          "[device-access][scanbench][sample]") {
  const TestMft::TempTreeImage image(TreeImage());
  const SampleRun run = RunEngine(L"--engine=all", {image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("Engine"));
  CHECK_THAT(run.out, ContainsSubstring("Wall (ms)"));
  CHECK(run.out.find("Directory of") == std::string::npos);
  CHECK(run.out.find("Record slots") == std::string::npos);

  const auto rows = TableRows(run.out);
  REQUIRE(rows.size() == 3);
  const std::vector<std::string> names{"mftlist", "nocache", "fullcache"};
  for (std::size_t index = 0; index < rows.size(); index++) {
    INFO("row " << names.at(index));
    const std::vector<std::string>& row = rows.at(index);
    REQUIRE(row.size() == 7);
    CHECK(row.at(0) == names.at(index));
    CHECK(row.at(1) == "0");
    CHECK(std::stoull(row.at(4)) > 0);
    CHECK(std::stoull(row.at(5)) > 0);
    CHECK(std::stoull(row.at(6)) > 0);
  }
  // Both walks count the same files and directories.
  CHECK(rows.at(1).at(4) == rows.at(2).at(4));
  CHECK(rows.at(1).at(4) == "5");
}

TEST_CASE("NtfsScanBench --engine=all takes --threads for the mftlist engine",
          "[device-access][scanbench][sample]") {
  const TestMft::TempTreeImage image(TreeImage());
  const SampleRun run =
      RunEngine(L"--engine=all", {L"--threads=2", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(TableRows(run.out).size() == 3);
}

TEST_CASE("NtfsScanBench --engine=all --quiet prints the same table",
          "[device-access][scanbench][sample]") {
  const TestMft::TempTreeImage image(TreeImage());
  const SampleRun run =
      RunEngine(L"--engine=all", {L"--quiet", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(TableRows(run.out).size() == 3);
}

TEST_CASE("NtfsScanBench --engine=all shows a failing engine in its row",
          "[device-access][scanbench][sample]") {
  const TempImage image(std::vector<BYTE>(4096, 0));
  const SampleRun run =
      RunEngine(L"--engine=all", {L"--log=console:off", image.path.wstring()});

  CHECK(run.exit_code == sample_failure_exit_code);
  const auto rows = TableRows(run.out);
  REQUIRE(rows.size() == 3);
  for (const auto& row : rows) {
    REQUIRE(row.size() == 7);
    CHECK(row.at(1) == "-1");
  }
}
