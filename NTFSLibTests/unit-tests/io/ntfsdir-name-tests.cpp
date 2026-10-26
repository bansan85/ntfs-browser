#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "long-path-support.h"
#include "name-support.h"
#include "ntfsdir-sample-support.h"
#include "sample-run.h"
#include "text-convert.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNameTree;
using NTFS_BROWSER_NAMESPACE::Tests::ExoticNames;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::FillerName;
using NTFS_BROWSER_NAMESPACE::Tests::LongTempDir;
using NTFS_BROWSER_NAMESPACE::Tests::MakeNameChain;
using NTFS_BROWSER_NAMESPACE::Tests::name_tree_root;
using NTFS_BROWSER_NAMESPACE::Tests::name_tree_write_time;
using NTFS_BROWSER_NAMESPACE::Tests::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::Below;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::DirectoryHeader;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::DocsListing;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::InImage;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::ListingLine;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::RootListing;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::RunDir;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::SampleExes;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::TempImage;
namespace Fs = std::filesystem;

namespace {

// The path of a directory that a recursive listing writes after its image: the
// names below it, each after a separator.
std::string RecursivePath(const std::string& image,
                          const std::vector<std::wstring>& names) {
  std::string path = image;
  for (const std::wstring& name : names) {
    path = Below(path, NtfsDevice::WideToUtf8(name));
  }
  return path;
}

// A node called name, a POSIX name: NTFS lets two of them differ only by case.
FakeTreeNode PosixName(std::wstring name, std::size_t parent,
                       bool directory = false) {
  return {
      .links = {{.name = std::move(name), .parent = parent, .name_space = 0}},
      .directory = directory};
}

// The image of a default TempImage: Docs in the root, report.txt in Docs.
std::vector<BYTE> ReportImage() {
  return NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithReportName(
      L"report.txt", NTFS_BROWSER_NAMESPACE::Tests::dump_broken_alter_time);
}

// The listing line of an entry of the name tree.
std::string Line(std::string_view name, bool directory = false,
                 std::string_view marker = {}) {
  return ListingLine(name, directory, name_tree_write_time, marker);
}

}  // namespace

TEST_CASE("NtfsDir prints an accented name as UTF-8", "[io][sample][name]") {
  const TempImage image(L"caf\u00E9.txt");
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("caf\xC3\xA9.txt"));
}

TEST_CASE("NtfsDir prints an unpaired surrogate in its WTF-8 form",
          "[io][sample][name]") {
  const TempImage image(std::wstring{static_cast<wchar_t>(0xD800), L'a'});
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("\xED\xA0\x80"
                               "a"));
}

TEST_CASE("NtfsDir prints the whole name when the code page lacks a character",
          "[io][sample][name]") {
  const TempImage image(
      L"ab\u65E5"
      L"c.txt");
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("ab\xE6\x97\xA5"
                               "c.txt"));
}

TEST_CASE("NtfsDir prints a character past the BMP as four bytes",
          "[io][sample][name]") {
  const TempImage image(
      L"a\U0001F600"
      L"b.txt");
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("a\xF0\x9F\x98\x80"
                               "b.txt"));
}

TEST_CASE("NtfsDir prints every kind of exotic name", "[io][sample][name]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Exotic"}}, .directory = true}};
  for (const auto& exotic : ExoticNames()) {
    nodes.push_back({.links = {{.name = exotic.name, .parent = 0}}});
  }
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));
  const SampleRun run = RunDir(exe, {InImage(image.path, L"Exotic")});

  CHECK(run.exit_code == 0);
  for (const auto& exotic : ExoticNames()) {
    INFO(exotic.label);
    CHECK_THAT(run.out, ContainsSubstring(Line(exotic.utf8)));
  }
  CHECK_THAT(run.out, ContainsSubstring(
                          "Files: " + std::to_string(ExoticNames().size()) +
                          ", Directories: 0\n"));
}

TEST_CASE(
    "NtfsDir lists names that differ only by case and enters both "
    "directories",
    "[io][sample][name][case]") {
  // A directory a and a directory A, each with a file x; three files that
  // differ by case; one file with the hard links x and X.
  const std::vector<FakeTreeNode> nodes{
      PosixName(L"readme", name_tree_root),
      PosixName(L"README", name_tree_root),
      PosixName(L"ReadMe", name_tree_root),
      PosixName(L"a", name_tree_root, true),
      PosixName(L"A", name_tree_root, true),
      PosixName(L"x", 3),
      PosixName(L"x", 4),
      {.links = {{.name = L"x", .name_space = 0},
                 {.name = L"X", .name_space = 0}}},
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));
  const std::string root = image.path.string();

  const SampleRun run = RunDir(NTFS_DIR_EXE, {L"-r", image.path.wstring()});

  // The index keeps names that are equal when upcased by code unit.
  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + Line("A", true) + Line("a", true) +
                       Line("README") + Line("ReadMe") + Line("readme") +
                       Line("X") + Line("x") +
                       DirectoryHeader(Below(root, "A")) + Line("x") +
                       DirectoryHeader(Below(root, "a")) + Line("x") +
                       "Files: 7, Directories: 2\n");
}

TEST_CASE("NtfsDir lists a freed Report.txt next to a live report.txt",
          "[io][sample][name][case]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Report.txt", .name_space = 0}},
       .in_use = false,
       .keep_entries = true},
      PosixName(L"report.txt", name_tree_root),
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun live = RunDir(NTFS_DIR_EXE, {image.path.wstring()});
  const SampleRun deleted =
      RunDir(NTFS_DIR_EXE, {L"--deleted", image.path.wstring()});

  CHECK(live.out == Line("report.txt") + "Files: 1, Directories: 0\n");
  CHECK(deleted.out == Line("Report.txt", false, "*") +
                           Line("report.txt", false, " ") +
                           "Files: 2, Directories: 0\n");
}

TEST_CASE("NtfsDir lists a tree whose paths pass 32767 units",
          "[io][sample][name]") {
  // 150 levels of names of 255 units: the deepest header is a path of 38 400
  // units, and each header names the whole path.
  constexpr std::size_t depth = 150;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(0x4E00 + level));
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(
      MakeNameChain(depth, name_of, L"leaf.txt")));
  std::vector<std::wstring> names;
  for (std::size_t level = 0; level < depth; level++) {
    names.push_back(name_of(level));
  }
  const std::string deepest = RecursivePath(image.path.string(), names);
  REQUIRE(deepest.size() > 32767);

  const SampleRun run =
      RunDir(NTFS_DIR_EXE, {L"--recursive", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out,
             ContainsSubstring(DirectoryHeader(deepest) + Line("leaf.txt") +
                               "Files: 1, Directories: 150\n"));
}

TEST_CASE("NtfsDir lists a tree of thousands of levels", "[io][sample][name]") {
  constexpr std::size_t depth = 2500;
  const TempImage image(BuildFakeNtfsImageWithNameTree(MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"d"); }, L"leaf.txt")));
  const std::string deepest =
      RecursivePath(image.path.string(),
                    std::vector<std::wstring>(depth, std::wstring(L"d")));

  const SampleRun run = RunDir(NTFS_DIR_EXE, {L"-r", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out,
             ContainsSubstring(DirectoryHeader(deepest) + Line("leaf.txt") +
                               "Files: 1, Directories: 2500\n"));
}

TEST_CASE("NtfsDir walks down to a target deeper than 260 characters",
          "[io][sample][name]") {
  constexpr std::size_t depth = 5;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(L'a' + level));
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(
      MakeNameChain(depth, name_of, L"leaf.txt")));
  std::wstring target = InImage(image.path, L"");
  for (std::size_t level = 0; level < depth; level++) {
    target += name_of(level);
    if (level + 1 < depth) {
      target += L"/";
    }
  }
  REQUIRE(target.size() > 260 * 4);

  const SampleRun run = RunDir(NTFS_DIR_EXE, {target});

  CHECK(run.exit_code == 0);
  CHECK(run.out == Line("leaf.txt") + "Files: 1, Directories: 0\n");
}

TEST_CASE("NtfsDir walks down to a name that ends with a dot or a space",
          "[io][sample][name]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"dot."}}, .directory = true},
      {.links = {{.name = L"space "}}, .directory = true},
      {.links = {{.name = L"inside dot", .parent = 0}}},
      {.links = {{.name = L"inside space", .parent = 1}}},
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun dot = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"dot.")});
  const SampleRun space =
      RunDir(NTFS_DIR_EXE, {InImage(image.path, L"space ")});

  CHECK(dot.exit_code == 0);
  CHECK(dot.out == Line("inside dot") + "Files: 1, Directories: 0\n");
  CHECK(space.exit_code == 0);
  CHECK(space.out == Line("inside space") + "Files: 1, Directories: 0\n");
}

TEST_CASE("NtfsDir walks down to a name with a character past the BMP",
          "[io][sample][name]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"d\U0001F600"
                          L"r"}},
       .directory = true},
      {.links = {{.name = L"inside", .parent = 0}}},
  };
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path,
                                                      L"d\U0001F600"
                                                      L"r")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == Line("inside") + "Files: 1, Directories: 0\n");
}

TEST_CASE("NtfsDir lists a long name once, whatever its DOS alias",
          "[io][sample][name]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"LongName.txt"},
                 {.name = L"LONGNA~1.TXT", .name_space = 2}}}};
  const TempImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun run = RunDir(NTFS_DIR_EXE, {image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == Line("LongName.txt") + "Files: 1, Directories: 0\n");
}

TEST_CASE("NtfsDir opens an image at a path of more than 260 characters",
          "[io][sample][long-path]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const LongTempDir directory(300);
  const Fs::path image = directory.Write("image.img", ReportImage());
  REQUIRE(image.native().size() > 260);

  const SampleRun root = RunDir(exe, {image.wstring()});
  const SampleRun docs = RunDir(exe, {InImage(image, L"Docs")});

  CHECK(root.exit_code == 0);
  CHECK(root.out == RootListing());
  CHECK(docs.exit_code == 0);
  CHECK(docs.out == DocsListing("report.txt"));
}

#ifdef _WIN32

TEST_CASE("NtfsDir opens an image through every form of a long path",
          "[io][sample][long-path]") {
  const LongTempDir directory(300);
  const Fs::path image = directory.Write("image.img", ReportImage());
  const std::wstring plain = image.wstring();
  std::wstring forward = plain;
  std::ranges::replace(forward, L'\\', L'/');

  for (const std::wstring& form :
       {plain, forward, LR"(\\?\)" + plain,
        directory.Directory().wstring() + LR"(\.\image.img)"}) {
    INFO(Fs::path(form).string().substr(0, 80));
    const SampleRun run = RunDir(NTFS_DIR_EXE, {form});

    CHECK(run.exit_code == 0);
    CHECK(run.out == RootListing());
  }
}

TEST_CASE("NtfsDir opens an image at a UNC path of more than 260 characters",
          "[io][sample][long-path]") {
  const LongTempDir directory(300);
  const Fs::path image = directory.Write("image.img", ReportImage());
  // The administrative share of the drive, as \\localhost\C$\...
  const std::wstring plain = image.wstring();
  const std::wstring unc =
      LR"(\\localhost\)" + plain.substr(0, 1) + L"$" + plain.substr(2);
  std::error_code error;
  if (!Fs::exists(Fs::path(LR"(\\?\UNC\)" + unc.substr(2)), error)) {
    SKIP("the administrative share is not reachable");
  }

  const SampleRun run = RunDir(NTFS_DIR_EXE, {unc});

  CHECK(run.exit_code == 0);
  CHECK(run.out == RootListing());
}

#else

TEST_CASE("NtfsDir reports a path longer than the system takes",
          "[io][sample][long-path]") {
  // PATH_MAX is 4096 bytes: nothing opens, and nothing crashes or cuts the
  // path short.
  const std::wstring path = L"/" + std::wstring(5000, L'p') + L"/image.img";

  const SampleRun run = RunDir(NTFS_DIR_EXE, {path});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "Cannot find a device or image file in " +
                       std::string(path.begin(), path.end()) + "\n");
}

#endif
