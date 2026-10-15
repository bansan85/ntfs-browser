#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include "entry.h"
#include "optional-access.h"
#include "os-api-walk.h"

using NTFS_BROWSER_NAMESPACE::Tests::Unwrap;
using NtfsCompare::Entry;
using NtfsCompare::Listing;
using NtfsCompare::WalkOsApi;
namespace Fs = std::filesystem;

namespace {

// A fresh directory under the temp path. Removed with everything in it on
// destruction.
class TempTree final {
 public:
  TempTree() {
    std::random_device random_device;
    root_ = Fs::temp_directory_path() /
            ("ntfscompare-os-walk-" + std::to_string(random_device()));
    Fs::create_directories(root_);
  }

  ~TempTree() {
    std::error_code ignored;
    Fs::remove_all(root_, ignored);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;
  TempTree(TempTree&&) = delete;
  TempTree& operator=(TempTree&&) = delete;

  [[nodiscard]] const Fs::path& Root() const { return root_; }

 private:
  Fs::path root_;
};

// Writes content to a new file at path.
void WriteFile(const Fs::path& path, std::string_view content) {
  std::ofstream(path, std::ios::binary) << content;
}

}  // namespace

TEST_CASE("WalkOsApi lists every entry under its relative path",
          "[ntfscompare][os-walk][characterization]") {
  const TempTree tree;
  WriteFile(tree.Root() / "a.txt", "hello");
  Fs::create_directories(tree.Root() / "dir" / "sub");
  WriteFile(tree.Root() / "dir" / "b.bin", "abc");
  WriteFile(tree.Root() / "dir" / "sub" / "c.txt", "");
  Fs::create_directories(tree.Root() / "empty");

  const Listing listing = WalkOsApi(tree.Root());

  CHECK(listing.size() == 6);
  for (const std::wstring_view key : {L"a.txt", L"dir", L"dir/b.bin",
                                      L"dir/sub", L"dir/sub/c.txt", L"empty"}) {
    CHECK(listing.contains(std::wstring(key)));
  }
}

TEST_CASE("WalkOsApi reports a file's size and a directory's kind",
          "[ntfscompare][os-walk][characterization]") {
  const TempTree tree;
  WriteFile(tree.Root() / "a.txt", "hello");
  Fs::create_directories(tree.Root() / "dir");

  const Listing listing = WalkOsApi(tree.Root());

  REQUIRE(listing.contains(L"a.txt"));
  REQUIRE(listing.contains(L"dir"));
  const Entry& file = listing.at(L"a.txt");
  const Entry& dir = listing.at(L"dir");
  CHECK_FALSE(file.is_directory);
  CHECK(Unwrap(file.logical_size) == 5ULL);
  CHECK(file.physical_size.has_value());
  CHECK(file.modification_time_utc.has_value());
  CHECK(dir.is_directory);
  CHECK_FALSE(dir.logical_size.has_value());
}

TEST_CASE("WalkOsApi of an empty directory lists nothing",
          "[ntfscompare][os-walk][characterization]") {
  const TempTree tree;

  CHECK(WalkOsApi(tree.Root()).empty());
}

TEST_CASE("WalkOsApi of a missing directory lists nothing",
          "[ntfscompare][os-walk][characterization]") {
  const TempTree tree;

  CHECK(WalkOsApi(tree.Root() / "no-such-dir").empty());
}

#ifdef _WIN32
TEST_CASE("WalkOsApi reports the read-only attribute of a file",
          "[ntfscompare][os-walk][characterization]") {
  const TempTree tree;
  const Fs::path locked = tree.Root() / "locked.txt";
  WriteFile(locked, "x");
  REQUIRE(SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_READONLY) != 0);

  const Listing listing = WalkOsApi(tree.Root());

  REQUIRE(listing.contains(L"locked.txt"));
  CHECK(Unwrap(listing.at(L"locked.txt").read_only));
  // Clears the attribute again, so the tree can be removed.
  SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_NORMAL);
}
#endif
