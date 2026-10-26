#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include "catch2/catch_message.hpp"
#include "compare-engine.h"
#include "entry.h"
#include "fake-ntfs-image.h"
#include "library-walk.h"
#include "long-path-support.h"
#include "name-support.h"
#include "name-tree-support.h"
#include "os-api-walk.h"
#include "sample-run.h"
#include "std-filesystem-walk.h"
#include "temp-dir.h"
#include "volume-open.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Fs = std::filesystem;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::FakeTreeNode;
using NTFS_BROWSER_NAMESPACE::Tests::FillerName;
using NTFS_BROWSER_NAMESPACE::Tests::LongTempDir;
using NTFS_BROWSER_NAMESPACE::Tests::MakeNameChain;
using NTFS_BROWSER_NAMESPACE::Tests::OpenNameTreeVolume;
using NtfsCompare::CompareAgainstReference;
using NtfsCompare::CompareLibraryMethods;
using NtfsCompare::comparison_volume_options;
using NtfsCompare::Entry;
using NtfsCompare::Listing;
using NtfsCompare::Report;
using NtfsCompare::ResolveDirectoryRecord;
using NtfsCompare::WalkLibraryIndex;
using NtfsCompare::WalkMftTree;
using NtfsCompare::WalkOsApi;
using NtfsCompare::WalkStdFilesystem;

namespace {

// The record of the root directory.
constexpr ULONGLONG root_record = 5;

// The keys of listing, in order.
std::set<std::wstring> KeysOf(const Listing& listing) {
  std::set<std::wstring> keys;
  for (const auto& [key, entry] : listing) {
    keys.insert(key);
  }
  return keys;
}

// A file entry of size bytes.
Entry FileOfSize(ULONGLONG size) {
  Entry entry;
  entry.logical_size = size;
  return entry;
}

// The tree of names that differ only by case: a directory a and a directory A,
// each with a file x; three files readme, README and ReadMe; one file with the
// hard links x and X; a freed Report.txt and a live report.txt.
std::vector<FakeTreeNode> TwinNodes() {
  return {
      {.links = {{.name = L"readme", .name_space = 0}}},
      {.links = {{.name = L"README", .name_space = 0}}},
      {.links = {{.name = L"ReadMe", .name_space = 0}}},
      {.links = {{.name = L"a", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"A", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"x", .parent = 3}}},
      {.links = {{.name = L"x", .parent = 4}}},
      {.links = {{.name = L"x", .name_space = 0},
                 {.name = L"X", .name_space = 0}}},
      {.links = {{.name = L"Report.txt", .name_space = 0}},
       .in_use = false,
       .keep_entries = true},
      {.links = {{.name = L"report.txt", .name_space = 0}}},
  };
}

// The three library listings of a tree of nodes, from the root.
struct LibraryListings {
  Listing full_cache;
  Listing no_cache;
  Listing mft_tree;
};

LibraryListings ListLibrary(const std::vector<FakeTreeNode>& nodes) {
  const auto full_cache = OpenNameTreeVolume<Cache::Strategy::FullCache>(
      nodes, comparison_volume_options);
  const auto no_cache = OpenNameTreeVolume<Cache::Strategy::NoCache>(
      nodes, comparison_volume_options);
  const MftTree tree(*no_cache);
  return {.full_cache = WalkLibraryIndex(*full_cache, root_record),
          .no_cache = WalkLibraryIndex(*no_cache, root_record),
          .mft_tree = WalkMftTree(tree, root_record)};
}

}  // namespace

TEST_CASE("the library walks list names that differ only by case, each once",
          "[ntfscompare][name-tree][case]") {
  const LibraryListings listings = ListLibrary(TwinNodes());

  const std::set<std::wstring> expected{
      L"readme", L"README", L"ReadMe", L"a", L"A",
      L"a/x",    L"A/x",    L"x",      L"X", L"report.txt"};
  CHECK(KeysOf(listings.full_cache) == expected);
  CHECK(KeysOf(listings.no_cache) == expected);
  CHECK(KeysOf(listings.mft_tree) == expected);
}

TEST_CASE("the library methods agree on a tree of names that differ by case",
          "[ntfscompare][name-tree][case]") {
  const LibraryListings listings = ListLibrary(TwinNodes());
  Report report;

  const Listing reference = CompareLibraryMethods(
      listings.full_cache, listings.no_cache, listings.mft_tree, report);

  CHECK(report.findings.empty());
  CHECK(reference.size() == 10);
}

TEST_CASE("CompareAgainstReference pairs names that differ only by case",
          "[ntfscompare][name-tree][case]") {
  const Listing reference{{L"readme", FileOfSize(5)},
                          {L"README", FileOfSize(7)}};
  Report matching;
  Report swapped;

  CompareAgainstReference(
      "os", reference,
      Listing{{L"readme", FileOfSize(5)}, {L"README", FileOfSize(7)}},
      matching);
  CompareAgainstReference(
      "os", reference,
      Listing{{L"readme", FileOfSize(7)}, {L"README", FileOfSize(5)}}, swapped);

  CHECK(matching.findings.empty());
  // Each name is paired with its own entry: both sizes differ.
  CHECK(swapped.findings.size() == 2);
}

TEST_CASE(
    "CompareAgainstReference reports a name the other side lacks, "
    "whatever the case of its twin",
    "[ntfscompare][name-tree][case]") {
  const Listing reference{{L"readme", FileOfSize(5)},
                          {L"README", FileOfSize(7)}};
  Report report;

  CompareAgainstReference("os", reference, Listing{{L"readme", FileOfSize(5)}},
                          report);

  REQUIRE(report.findings.size() == 1);
  CHECK(report.findings.front().kind == "MISSING");
  CHECK(report.findings.front().path == L"README");
}

TEST_CASE(
    "the library walks list a chain of 255-unit names deeper than 32767 "
    "units",
    "[ntfscompare][name-tree]") {
  constexpr std::size_t depth = 140;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(0x4E00 + level));
  };
  const std::vector<FakeTreeNode> nodes =
      MakeNameChain(depth, name_of, L"leaf.txt");
  std::wstring deepest;
  for (std::size_t level = 0; level < depth; level++) {
    deepest += (level == 0 ? L"" : L"/") + name_of(level);
  }
  REQUIRE(deepest.size() > 32767);

  const LibraryListings listings = ListLibrary(nodes);

  for (const Listing* listing :
       {&listings.full_cache, &listings.no_cache, &listings.mft_tree}) {
    CHECK(listing->size() == depth + 1);
    CHECK(listing->contains(deepest));
    CHECK(listing->contains(deepest + L"/leaf.txt"));
  }
}

TEST_CASE("the library walks list a chain of thousands of levels",
          "[ntfscompare][name-tree]") {
  constexpr std::size_t depth = 3000;
  const std::vector<FakeTreeNode> nodes = MakeNameChain(
      depth, [](std::size_t) { return std::wstring(L"d"); }, L"leaf.txt");

  const LibraryListings listings = ListLibrary(nodes);

  for (const Listing* listing :
       {&listings.full_cache, &listings.no_cache, &listings.mft_tree}) {
    CHECK(listing->size() == depth + 1);
  }
  CHECK(KeysOf(listings.no_cache) == KeysOf(listings.mft_tree));
  CHECK(KeysOf(listings.no_cache) == KeysOf(listings.full_cache));
}

TEST_CASE("ResolveDirectoryRecord walks down a path of more than 260 units",
          "[ntfscompare][name-tree]") {
  constexpr std::size_t depth = 5;
  const auto name_of = [](std::size_t level) {
    return FillerName(255, L'n', static_cast<wchar_t>(L'a' + level));
  };
  const std::vector<FakeTreeNode> nodes =
      MakeNameChain(depth, name_of, L"leaf.txt");
  const auto volume = OpenNameTreeVolume<Cache::Strategy::NoCache>(nodes);
  std::wstring path;
  for (std::size_t level = 0; level < depth; level++) {
    path += (level == 0 ? L"" : L"/") + name_of(level);
  }

  CHECK(ResolveDirectoryRecord(*volume, path) ==
        NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord(depth - 1));
  CHECK_FALSE(ResolveDirectoryRecord(*volume, path + L"/leaf.txt"));
}

TEST_CASE(
    "ResolveDirectoryRecord tells directories that differ only by case "
    "apart",
    "[ntfscompare][name-tree][case]") {
  const auto volume = OpenNameTreeVolume<Cache::Strategy::NoCache>(TwinNodes());

  CHECK(ResolveDirectoryRecord(*volume, L"a") ==
        NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord(3));
  CHECK(ResolveDirectoryRecord(*volume, L"A") ==
        NTFS_BROWSER_NAMESPACE::Tests::NameTreeRecord(4));
  CHECK(ResolveDirectoryRecord(*volume, L"A/x") == std::nullopt);
}

TEST_CASE("the library walks list a long name once, whatever its DOS alias",
          "[ntfscompare][name-tree]") {
  const std::vector<FakeTreeNode> nodes{
      {.links = {{.name = L"LongName.txt"},
                 {.name = L"LONGNA~1.TXT", .name_space = 2}}}};

  const LibraryListings listings = ListLibrary(nodes);

  const std::set<std::wstring> expected{L"LongName.txt"};
  CHECK(KeysOf(listings.full_cache) == expected);
  CHECK(KeysOf(listings.no_cache) == expected);
  CHECK(KeysOf(listings.mft_tree) == expected);
}

namespace {

// Writes content to a new file at path, which may be longer than 260 units.
void WriteHostFile(const Fs::path& path, std::size_t size) {
  std::ofstream output(NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path),
                       std::ios::binary);
  output << std::string(size, 'x');
}

// Makes the directory at path, which may be longer than 260 units.
void MakeHostDirectory(const Fs::path& path) {
  std::error_code error;
  Fs::create_directories(
      NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path), error);
  REQUIRE_FALSE(error);
}

}  // namespace

TEST_CASE("the operating-system walks list a tree deeper than 260 units",
          "[ntfscompare][os-walk][long-path]") {
  const LongTempDir directory(300);
  const Fs::path inner = directory.Directory() / std::string(100, 'p') /
                         std::string(100, 'q') / std::string(100, 'r');
  MakeHostDirectory(inner);
  WriteHostFile(inner / "deep.txt", 5);
  WriteHostFile(directory.Directory() / "top.txt", 3);
  REQUIRE(inner.native().size() > 600);

  const std::wstring below = std::wstring(100, L'p') + L"/" +
                             std::wstring(100, L'q') + L"/" +
                             std::wstring(100, L'r');
  const std::set<std::wstring> expected{L"top.txt", std::wstring(100, L'p'),
                                        std::wstring(100, L'p') + L"/" +
                                            std::wstring(100, L'q'),
                                        below, below + L"/deep.txt"};

  const Listing os = WalkOsApi(directory.Directory());
  const Listing standard = WalkStdFilesystem(directory.Directory());

  CHECK(KeysOf(os) == expected);
  CHECK(KeysOf(standard) == expected);
  REQUIRE(os.contains(below + L"/deep.txt"));
  CHECK(os.at(below + L"/deep.txt").logical_size == 5);
  CHECK(os.at(below + L"/deep.txt").physical_size.has_value());
  CHECK(os.at(L"top.txt").physical_size.has_value());
}

#ifdef _WIN32

namespace {

// Creates the file at path with size bytes, by its exact name: POSIX semantics
// look names up as they are spelled, and the extended-length prefix, given
// here, turns the normalisation of dots and spaces off. Returns whether it
// could.
bool CreateExactFile(const std::wstring& extended_path, std::size_t size) {
  const HANDLE handle =
      CreateFileW(extended_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const std::string content(size, 'x');
  DWORD written = 0;
  const bool done =
      WriteFile(handle, content.data(), static_cast<DWORD>(content.size()),
                &written, nullptr) != FALSE;
  CloseHandle(handle);
  return done;
}

// Deletes the file at path by its exact name.
void DeleteExactFile(const std::wstring& extended_path) {
  const HANDLE handle = CreateFileW(
      extended_path.c_str(), DELETE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_POSIX_SEMANTICS,
      nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle);
  }
}

// The extended-length form of the file called name in directory, built by
// hand: the name keeps its trailing dot or space.
std::wstring ExactPath(const Fs::path& directory, const std::wstring& name) {
  return LR"(\\?\)" + directory.wstring() + L"\\" + name;
}

}  // namespace

TEST_CASE(
    "the operating-system walks list the names Win32 cannot open as "
    "typed",
    "[ntfscompare][os-walk][long-path]") {
  const LongTempDir directory(300);
  const std::vector<std::wstring> names{L"dot.",       L"space ", L"CON",
                                        L"NUL.txt",    L"aux",    L"COM1.log",
                                        L"trailing. ."};
  for (const std::wstring& name : names) {
    REQUIRE(CreateExactFile(ExactPath(directory.Directory(), name), 1));
  }

  const Listing os = WalkOsApi(directory.Directory());
  const Listing standard = WalkStdFilesystem(directory.Directory());

  const std::set<std::wstring> expected(names.begin(), names.end());
  CHECK(KeysOf(os) == expected);
  CHECK(KeysOf(standard) == expected);
  for (const std::wstring& name : names) {
    INFO(Fs::path(name).string());
    REQUIRE(os.contains(name));
    CHECK(os.at(name).logical_size == 1);
    CHECK(os.at(name).physical_size.has_value());
  }

  for (const std::wstring& name : names) {
    DeleteExactFile(ExactPath(directory.Directory(), name));
  }
}

TEST_CASE("the operating-system walks list names that differ only by case",
          "[ntfscompare][os-walk][case]") {
  const NTFS_BROWSER_NAMESPACE::Tests::TempDir directory;
  const Fs::path folder = directory.Path() / "sensitive";
  MakeHostDirectory(folder);
  // A folder that keeps two names apart: the attribute NTFS gives a folder
  // since Windows 10 1803.
  std::array<wchar_t, MAX_PATH> system_directory{};
  GetSystemDirectoryW(system_directory.data(),
                      static_cast<UINT>(system_directory.size()));
  const Fs::path fsutil = Fs::path(system_directory.data()) / L"fsutil.exe";
  if (!Fs::exists(fsutil)) {
    SKIP("fsutil is not there");
  }
  const NTFS_BROWSER_NAMESPACE::Tests::SampleRun enabled =
      NTFS_BROWSER_NAMESPACE::Tests::RunSample(
          fsutil,
          {L"file", L"setCaseSensitiveInfo", folder.wstring(), L"enable"});
  if (enabled.exit_code != 0) {
    SKIP("this volume cannot make a folder case-sensitive");
  }
  const std::wstring lower = ExactPath(folder, L"twin");
  const std::wstring upper = ExactPath(folder, L"TWIN");
  // Sizes that take different numbers of clusters, so the allocated size of
  // each name is its own.
  REQUIRE(CreateExactFile(lower, 5000));
  REQUIRE(CreateExactFile(upper, 9000));

  const Listing os = WalkOsApi(folder);
  const Listing standard = WalkStdFilesystem(folder);

  CHECK(KeysOf(os) == std::set<std::wstring>{L"twin", L"TWIN"});
  CHECK(KeysOf(standard) == std::set<std::wstring>{L"twin", L"TWIN"});
  REQUIRE(os.contains(L"twin"));
  REQUIRE(os.contains(L"TWIN"));
  CHECK(os.at(L"twin").logical_size == 5000);
  CHECK(os.at(L"TWIN").logical_size == 9000);
  // Each name is opened as spelled: its allocation is its own.
  CHECK(os.at(L"twin").physical_size != os.at(L"TWIN").physical_size);
  CHECK(os.at(L"twin").physical_size.value_or(0) >= 5000);
  CHECK(os.at(L"TWIN").physical_size.value_or(0) >= 9000);

  DeleteExactFile(lower);
  DeleteExactFile(upper);
}

#else

TEST_CASE("the operating-system walks list names that differ only by case",
          "[ntfscompare][os-walk][case]") {
  const LongTempDir directory(300);
  const Fs::path lower = directory.Directory() / "twin";
  const Fs::path upper = directory.Directory() / "TWIN";
  WriteHostFile(lower, 5);
  WriteHostFile(upper, 9);
  std::error_code error;
  if (Fs::file_size(lower, error) != 5) {
    SKIP("this file system does not keep two names that differ only by case");
  }

  const Listing os = WalkOsApi(directory.Directory());
  const Listing standard = WalkStdFilesystem(directory.Directory());

  CHECK(KeysOf(os) == std::set<std::wstring>{L"twin", L"TWIN"});
  CHECK(KeysOf(standard) == std::set<std::wstring>{L"twin", L"TWIN"});
  CHECK(os.at(L"twin").logical_size == 5);
  CHECK(os.at(L"TWIN").logical_size == 9);
}

#endif  // _WIN32
