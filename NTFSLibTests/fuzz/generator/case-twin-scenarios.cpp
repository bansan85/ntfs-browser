#include "case-twin-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeNameTreeOptions;
using FakeImage::FakeTreeNode;

// How many fillers the two block inputs add: enough to push the twins from
// block to block, so that the separators of the root fall on each of them in
// turn.
constexpr std::array<std::size_t, 2> filler_counts{6, 13};

// Code units of a filler name, and the letter it is made of.
constexpr std::size_t filler_units = 20;
constexpr wchar_t filler_letter = L'a';

// A name of exactly units code units: fill repeated, then tag as the last one.
std::wstring FillerName(std::size_t units, wchar_t fill, wchar_t tag) {
  std::wstring name(units, fill);
  if (units != 0) {
    name.back() = tag;
  }
  return name;
}

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

// The twins, then fillers that sort before them.
std::vector<FakeTreeNode> TwinsAfterFillers(std::size_t fillers) {
  std::vector<FakeTreeNode> nodes = Twins();
  for (std::size_t index = 0; index < fillers; index++) {
    nodes.push_back(
        PosixFile(FillerName(filler_units, filler_letter,
                             static_cast<wchar_t>(filler_letter + index)) +
                  std::to_wstring(index)));
  }
  return nodes;
}

}  // namespace

void CaseTwinScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown, as the other families are.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded(
      "case_twin_root",
      [] { return FakeImage::BuildFakeNtfsImageWithNameTree(Twins()); },
      options);
  for (const std::size_t fillers : filler_counts) {
    registry.AddRecorded(
        "case_twin_blocks_" + std::to_string(fillers),
        [fillers] {
          return FakeImage::BuildFakeNtfsImageWithNameTree(
              TwinsAfterFillers(fillers));
        },
        options);
  }

  // The index still names a freed Report.txt, which sorts before the live
  // report.txt.
  registry.AddRecorded(
      "case_twin_stale_exact",
      [] {
        return FakeImage::BuildFakeNtfsImageWithNameTree(
            std::vector<FakeTreeNode>{
                {.links = {{.name = L"Report.txt", .name_space = 0}},
                 .in_use = false,
                 .keep_entries = true},
                PosixFile(L"report.txt")});
      },
      options);

  // The entries are not in collation order: the search gives up early, and the
  // scan that follows takes the exact spelling.
  registry.AddRecorded(
      "case_twin_unordered",
      [] {
        return FakeImage::BuildFakeNtfsImageWithNameTree(
            std::vector<FakeTreeNode>{PosixFile(L"zzz"), PosixFile(L"README"),
                                      PosixFile(L"readme")},
            FakeNameTreeOptions{.collate = false});
      },
      options);

  // One file with the hard links x and X.
  registry.AddRecorded(
      "case_twin_hard_links",
      [] {
        return FakeImage::BuildFakeNtfsImageWithNameTree(
            std::vector<FakeTreeNode>{
                {.links = {{.name = L"x", .name_space = 0},
                           {.name = L"X", .name_space = 0}}}});
      },
      options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
