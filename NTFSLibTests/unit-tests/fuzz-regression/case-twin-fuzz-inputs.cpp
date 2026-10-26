#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"
#include "name-support.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeNameTreeOptions;
using Tests::FakeTreeNode;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// A file node called name, a POSIX name: NTFS lets two of them differ only by
// case.
FakeTreeNode PosixFile(std::wstring name) {
  return {.links = {{.name = std::move(name), .name_space = 0}}};
}

// Three files that differ only by case, and two directories that do.
std::vector<FakeTreeNode> Twins() {
  return {
      PosixFile(L"readme"),
      PosixFile(L"README"),
      PosixFile(L"ReadMe"),
      {.links = {{.name = L"a", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"A", .name_space = 0}}, .directory = true},
      {.links = {{.name = L"x", .parent = 3}}},
      {.links = {{.name = L"x", .parent = 4}}},
  };
}

// The inputs, one per way the index can hold names that are equal when
// upcased.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back(
      {"case_twin_root", Tests::BuildFakeNtfsImageWithNameTree(Twins())});

  // Fillers that sort before the twins push them from block to block: the
  // separators of the root then fall on each of them in turn.
  for (const std::size_t fillers : {std::size_t{6}, std::size_t{13}}) {
    std::vector<FakeTreeNode> nodes = Twins();
    for (std::size_t index = 0; index < fillers; index++) {
      nodes.push_back(PosixFile(
          Tests::FillerName(20, L'a', static_cast<wchar_t>(L'a' + index)) +
          std::to_wstring(index)));
    }
    variants.push_back({"case_twin_blocks_" + std::to_string(fillers),
                        Tests::BuildFakeNtfsImageWithNameTree(nodes)});
  }

  // The index still names a freed Report.txt, which sorts before the live
  // report.txt.
  variants.push_back(
      {"case_twin_stale_exact",
       Tests::BuildFakeNtfsImageWithNameTree(std::vector<FakeTreeNode>{
           {.links = {{.name = L"Report.txt", .name_space = 0}},
            .in_use = false,
            .keep_entries = true},
           PosixFile(L"report.txt")})});

  // The entries are not in collation order: the search gives up early, and the
  // scan that follows takes the exact spelling.
  variants.push_back(
      {"case_twin_unordered",
       Tests::BuildFakeNtfsImageWithNameTree(
           std::vector<FakeTreeNode>{PosixFile(L"zzz"), PosixFile(L"README"),
                                     PosixFile(L"readme")},
           FakeNameTreeOptions{.collate = false})});

  // One file with the hard links x and X.
  variants.push_back(
      {"case_twin_hard_links",
       Tests::BuildFakeNtfsImageWithNameTree(std::vector<FakeTreeNode>{
           {.links = {{.name = L"x", .name_space = 0},
                      {.name = L"X", .name_space = 0}}}})});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs where the root holds names that are equal when
// upcased. The volume is opened recovering with deleted items shown, as the
// other families are. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write
// them in (the repository's NTFSLibTests/fuzz/data) and run this hidden test
// case by its tag.
TEST_CASE("write the case twin fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(variant.image), recovering);
  }
}
