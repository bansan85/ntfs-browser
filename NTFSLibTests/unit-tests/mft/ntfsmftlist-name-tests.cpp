#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "cluster-account-support.h"
#include "fake-ntfs-image.h"
#include "long-path-support.h"
#include "name-support.h"
#include "ntfsmftlist-sample-support.h"
#include "sample-run.h"
#include "text-convert.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithNameTree;
using NTFS_BROWSER_NAMESPACE::Tests::ExoticNames;
using NTFS_BROWSER_NAMESPACE::Tests::FakeDataRun;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::FillerName;
using NTFS_BROWSER_NAMESPACE::Tests::LongTempDir;
using NTFS_BROWSER_NAMESPACE::Tests::MakeNameChain;
using NTFS_BROWSER_NAMESPACE::Tests::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
using NTFS_BROWSER_NAMESPACE::Tests::MftListSample::RunMftList;
using NTFS_BROWSER_NAMESPACE::Tests::MftListSample::TempTreeImage;
namespace Fs = std::filesystem;

namespace {

// Spaces before a line that follows an entry, such as the "#" lines.
constexpr std::size_t path_column = 63;

// The end of the columns of an entry, and the start of its path: the hard link
// count, and the space after it.
std::string BeforePath(int links = 1) {
  return "  " + std::to_string(links) + " ";
}

// The path of the names below the root, each after a backslash, in UTF-8.
std::string PathOf(const std::vector<std::wstring>& names) {
  std::string path;
  for (const std::wstring& name : names) {
    path += '\\';
    path += NtfsDevice::WideToUtf8(name);
  }
  return path;
}

// The line of an entry that follows another line of the listing.
std::string Listed(const std::string& path, int links = 1) {
  return BeforePath(links) + path + "\n";
}

// How many times text holds part.
std::size_t Count(const std::string& text, const std::string& part) {
  std::size_t count = 0;
  for (std::size_t at = text.find(part); at != std::string::npos;
       at = text.find(part, at + part.size())) {
    count++;
  }
  return count;
}

}  // namespace

TEST_CASE("NtfsMftList prints the path of every kind of exotic name",
          "[mft][sample][name]") {
  std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"Exotic"}}, .directory = true}};
  for (const auto& exotic : ExoticNames()) {
    nodes.push_back({.links = {{.name = exotic.name, .parent = 0}}});
  }
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun run = RunMftList({image.path.wstring()});

  CHECK(run.exit_code == 0);
  for (const auto& exotic : ExoticNames()) {
    INFO(exotic.label);
    CHECK_THAT(run.out, ContainsSubstring(Listed("\\Exotic\\" + exotic.utf8)));
  }
}

TEST_CASE("NtfsMftList lists names that differ only by case, each once",
          "[mft][sample][name][case]") {
  // 0-2 three files; 3 and 4 the directories a and A, with a file x each; 7 a
  // file with the hard links x and X; 8 and 9 a freed Report.txt and a live
  // report.txt.
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
      {.links = {{.name = L"Report.txt", .name_space = 0}}, .in_use = false},
      {.links = {{.name = L"report.txt", .name_space = 0}}},
  };
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun run = RunMftList({L"--deleted", image.path.wstring()});

  CHECK(run.exit_code == 0);
  for (const char* path : {"\\readme", "\\README", "\\ReadMe", "\\a", "\\A",
                           "\\a\\x", "\\A\\x", "\\report.txt"}) {
    INFO(path);
    CHECK(Count(run.out, Listed(path)) == 1);
  }
  // A freed record shows DEL, and its name is spelled as it is.
  CHECK_THAT(run.out, ContainsSubstring("DEL"));
  CHECK_THAT(run.out, ContainsSubstring(Listed("\\Report.txt")));
  // One record, two names: the second is a line of its own.
  CHECK_THAT(run.out, ContainsSubstring(Listed("\\x", 2)));
  CHECK_THAT(run.out, ContainsSubstring("= \\X\n"));
}

TEST_CASE("NtfsMftList prints a path of more than 32767 units",
          "[mft][sample][name]") {
  // 200 levels of names of 255 units: the leaf path has about 51 000 units.
  constexpr std::size_t depth = 200;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(0x4E00 + level));
  };
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(
      MakeNameChain(depth, name_of, FillerName(255, L'f', L'!'))));
  std::vector<std::wstring> names;
  for (std::size_t level = 0; level < depth; level++) {
    names.push_back(name_of(level));
  }
  const std::string directory = PathOf(names);
  names.push_back(FillerName(255, L'f', L'!'));
  const std::string leaf = PathOf(names);
  REQUIRE(leaf.size() > 32767);

  const SampleRun run = RunMftList({image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring(Listed(leaf)));
  CHECK_THAT(run.out, ContainsSubstring(Listed(directory)));
}

TEST_CASE("NtfsMftList prints the paths of a chain of thousands of levels",
          "[mft][sample][name]") {
  // Paths past 4096 bytes are not kept by the listing: each is built again.
  constexpr std::size_t depth = 2300;
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"d"); }, L"leaf.txt")));
  std::vector<std::wstring> names(depth, std::wstring(L"d"));
  const std::string directory = PathOf(names);
  names.push_back(L"leaf.txt");

  const SampleRun run = RunMftList({image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring(Listed(PathOf(names))));
  CHECK_THAT(run.out, ContainsSubstring(Listed(directory)));
  CHECK(Count(run.out, "<DIR>") == depth + 1);
}

TEST_CASE("NtfsMftList puts the cluster lines under a path of 32767 units",
          "[mft][sample][name][cluster]") {
  // The leaf is a freed file with clusters, 200 levels down.
  constexpr std::size_t depth = 200;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(0x4E00 + level));
  };
  std::vector<FakeTreeNode> nodes =
      MakeNameChain(depth, name_of, FillerName(255, L'f', L'!'));
  nodes.back().in_use = false;
  nodes.back().runs = {FakeDataRun{.lcn = 4, .clusters = 2}};
  nodes.back().stream_size = 2048;
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(nodes));
  std::vector<std::wstring> names;
  for (std::size_t level = 0; level < depth; level++) {
    names.push_back(name_of(level));
  }
  names.push_back(FillerName(255, L'f', L'!'));
  const std::string leaf = PathOf(names);

  const SampleRun run =
      RunMftList({L"--deleted", L"--clusters=map", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out,
             ContainsSubstring(Listed(leaf) + std::string(path_column, ' ') +
                               "# clusters: "));
  CHECK_THAT(run.out, ContainsSubstring(std::string(path_column, ' ') +
                                        "# stream (unnamed): 0..2048 "));
}

TEST_CASE("NtfsMftList prints the name of a stream in every exotic form",
          "[mft][sample][name][cluster]") {
  namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
  for (const auto& exotic : ExoticNames()) {
    INFO(exotic.label);
    Support::Volume volume;
    auto stream = Support::Stream({{40, 4}}, 4096);
    stream.name = exotic.name;
    volume.Put(22, Support::Freed(100, stream));
    const TempTreeImage image(volume.Build());

    const SampleRun run =
        RunMftList({L"--deleted", L"--clusters=map", image.path.wstring()});

    CHECK(run.exit_code == 0);
    CHECK_THAT(run.out, ContainsSubstring(std::string(path_column, ' ') +
                                          "# stream " + exotic.utf8 + ":"));
  }
}

TEST_CASE("NtfsMftList prints the name of a long stream",
          "[mft][sample][name][cluster]") {
  namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
  // As long as a record of the fixture holds before its first sector ends.
  const std::wstring name = FillerName(60, L's', L'é');
  Support::Volume volume;
  auto stream = Support::Stream({{40, 4}}, 4096);
  stream.name = name;
  volume.Put(22, Support::Freed(100, stream));
  const TempTreeImage image(volume.Build());

  const SampleRun run =
      RunMftList({L"--deleted", L"--clusters=map", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("# stream " +
                                        NtfsDevice::WideToUtf8(name) + ":"));
}

TEST_CASE("NtfsMftList lists a long name once, whatever its DOS alias",
          "[mft][sample][name]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"LongName.txt"},
                 {.name = L"LONGNA~1.TXT", .name_space = 2}}}};
  const TempTreeImage image(BuildFakeNtfsImageWithNameTree(nodes));

  const SampleRun run = RunMftList({image.path.wstring()});

  CHECK(run.exit_code == 0);
  // The alias is not a hard link: one name, and no line of its own.
  CHECK_THAT(run.out, ContainsSubstring(Listed("\\LongName.txt")));
  CHECK_THAT(run.out, !ContainsSubstring("LONGNA~1"));
}

TEST_CASE("NtfsMftList lists an image at a path of more than 260 characters",
          "[mft][sample][long-path]") {
  const LongTempDir directory(300);
  const Fs::path image =
      directory.Write("image.img", BuildFakeNtfsImageWithMftTree());
  REQUIRE(image.native().size() > 260);
  const TempTreeImage short_path;

  const SampleRun long_run = RunMftList({image.wstring()});
  const SampleRun short_run = RunMftList({short_path.path.wstring()});

  CHECK(long_run.exit_code == 0);
  CHECK(long_run.out == short_run.out);
}

#ifdef _WIN32

TEST_CASE("NtfsMftList lists an image through every form of a long path",
          "[mft][sample][long-path]") {
  const LongTempDir directory(300);
  const Fs::path image =
      directory.Write("image.img", BuildFakeNtfsImageWithMftTree());
  const TempTreeImage short_path;
  const SampleRun expected = RunMftList({short_path.path.wstring()});
  const std::wstring plain = image.wstring();
  std::wstring forward = plain;
  std::ranges::replace(forward, L'\\', L'/');

  for (const std::wstring& form :
       {plain, forward, LR"(\\?\)" + plain,
        directory.Directory().wstring() + LR"(\.\image.img)"}) {
    INFO(Fs::path(form).string().substr(0, 80));
    const SampleRun run = RunMftList({form});

    CHECK(run.exit_code == 0);
    CHECK(run.out == expected.out);
  }
}

#else

TEST_CASE("NtfsMftList reports a path longer than the system takes",
          "[mft][sample][long-path]") {
  const std::wstring path = L"/" + std::wstring(5000, L'p') + L"/image.img";

  const SampleRun run = RunMftList({path});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK_THAT(run.err, ContainsSubstring("as an NTFS volume"));
}

#endif
