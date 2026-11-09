#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/mft/ntfsscanbench-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>
#include <ntfs-browser/tool/device-access/json.h>

#include "win-types.h"

namespace TestDeviceAccess = NTFS_BROWSER_NAMESPACE::Test::DeviceAccess;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestMft = NTFS_BROWSER_NAMESPACE::Test::Mft;
namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Test::Support::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Test::Support::SampleRun;
using ToolDeviceAccess::JsonValue;

namespace {

// A tree with a Docs folder: a.txt and c.txt in it, and root.txt in the root.
std::vector<BYTE> TreeImage() {
  using TestFake::FakeTreeNode;
  const auto file = [](std::wstring name, std::size_t parent, DWORD size) {
    return FakeTreeNode{.links = {{.name = std::move(name), .parent = parent}},
                        .data_size = size,
                        .index_size = size};
  };
  constexpr std::size_t docs = 0;
  std::vector<FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"Docs"}}, .directory = true});
  nodes.push_back(file(L"a.txt", docs, 10));
  nodes.push_back(file(L"c.txt", docs, 5));
  nodes.push_back(file(L"root.txt", TestFake::name_tree_root, 7));
  return TestFake::BuildFakeNtfsImageWithNameTree(nodes);
}

// The lines of text that start with a name, in order: the rows of the engine
// that the table lists.
std::vector<std::string> LinesStartingWith(const std::string& text,
                                           std::string_view name) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string line = text.substr(start, end - start);
    if (line.starts_with(name)) {
      lines.push_back(line);
    }
    start = end + 1;
  }
  return lines;
}

}  // namespace

TEST_CASE("NtfsScanBench --via-scan --engine=all adds the worker tables",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());

  const SampleRun run = TestMft::RunScanBench(
      {L"--engine=all", L"--via-scan", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("Wall (ms)"));
  CHECK_THAT(run.out, ContainsSubstring("Verdicts"));
  CHECK_THAT(run.out, ContainsSubstring("Worker thread of the scan"));
  CHECK_THAT(run.out, ContainsSubstring("Parallelism"));
  for (const std::string_view engine : {"mftlist", "nocache", "fullcache"}) {
    INFO(engine);
    // The engine is a row of the table, of the verdicts, of the worker and of
    // the parallelism.
    CHECK(LinesStartingWith(run.out, engine).size() == 4);
  }
}

TEST_CASE("NtfsScanBench without --via-scan prints no worker table",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());

  const SampleRun run =
      TestMft::RunScanBench({L"--engine=all", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out.find("Verdicts") == std::string::npos);
  CHECK(run.out.find("Worker thread") == std::string::npos);
}

TEST_CASE("NtfsScanBench --via-scan runs one engine with a pattern",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());

  const SampleRun run =
      TestMft::RunScanBench({L"--engine=auto", L"--via-scan", L"--name=*.txt",
                             L"--min-size=6", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("Worker thread of the scan"));
  CHECK(LinesStartingWith(run.out, "auto").size() == 3);
}

TEST_CASE("NtfsScanBench --via-scan takes a regular expression",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());

  const SampleRun run =
      TestMft::RunScanBench({L"--engine=fullcache", L"--via-scan",
                             L"--regex=^[ac]\\.txt$", image.path.wstring()});

  if (!NTFS_BROWSER_NAMESPACE::Search::RegexAvailable()) {
    CHECK(run.exit_code == sample_failure_exit_code);
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(LinesStartingWith(run.out, "fullcache").size() == 3);
}

TEST_CASE("NtfsScanBench refuses the options of a scan without --via-scan",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());
  for (const std::wstring option :
       {L"--engine=auto", L"--name=*.txt", L"--min-size=1"}) {
    const SampleRun run = TestMft::RunScanBench({option, image.path.wstring()});

    CHECK(run.exit_code == sample_failure_exit_code);
    CHECK_THAT(run.err, ContainsSubstring("need --via-scan"));
  }
}

TEST_CASE("NtfsScanBench refuses a pattern that is empty or doubled",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());
  const std::vector<std::vector<std::wstring>> bad{
      {L"--name="}, {L"--name=a", L"--regex=a"}, {L"--min-size=x"}};
  for (std::vector<std::wstring> args : bad) {
    args.insert(args.begin(), L"--via-scan");
    args.push_back(image.path.wstring());

    const SampleRun run = TestMft::RunScanBench(args);

    CHECK(run.exit_code == sample_failure_exit_code);
  }
}

TEST_CASE("NtfsScanBench --via-scan --benchmark adds a scan member",
          "[device-access][scanbench][sample][viascan]") {
  const TestMft::TempTreeImage image(TreeImage());
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path summary = dir.Path() / "scan.json";

  const SampleRun run = TestMft::RunScanBench(
      {L"--via-scan", L"--engine=mftlist", L"--benchmark=" + summary.wstring(),
       image.path.wstring()});

  REQUIRE(run.exit_code == 0);
  std::ifstream in(summary, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  std::string error;
  const std::optional<JsonValue> root =
      ToolDeviceAccess::ParseJson(text, error);
  INFO(error);
  REQUIRE(root.has_value());
  const JsonValue* const scan = root->Find("scan");
  REQUIRE(scan != nullptr);
  for (const std::string_view member :
       {"entries", "wall_ns", "worker", "figures", "verdict"}) {
    INFO(member);
    CHECK(scan->Find(member) != nullptr);
  }
  const JsonValue* const verdict = scan->Find("verdict");
  REQUIRE(verdict != nullptr);
  CHECK(verdict->Find("critical_path") != nullptr);
  CHECK(verdict->Find("parallel_efficiency") != nullptr);
  CHECK(root->Find("run") != nullptr);
}
