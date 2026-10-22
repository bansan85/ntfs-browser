#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// One FullCache block: the smallest ceiling that still caches a block.
constexpr std::uint64_t one_block_ceiling = 65536;

// A ceiling the small fixture never reaches.
constexpr std::uint64_t roomy_ceiling = 16ULL * 1024 * 1024;

// The volume over the MftTree fixture, with the given ceiling. include_deleted
// keeps the fixture's freed records in the tree, so the comparison covers them.
template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>> OpenTreeVolume(std::uint64_t max_cache_bytes) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree()),
      VolumeOptions{.include_deleted = true,
                    .max_cache_bytes = max_cache_bytes});
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// One line per entry, with its path and the fields a cache fault would damage,
// then the scan statistics.
std::vector<std::wstring> Describe(const MftTree& tree) {
  std::vector<std::wstring> lines;
  for (const MftTree::Entry& entry : tree.Entries()) {
    std::wstring line =
        std::to_wstring(entry.record) + L"|" + std::to_wstring(entry.sequence) +
        L"|" + tree.GetPath(entry.record) + L"|" +
        std::to_wstring(entry.in_use) + std::to_wstring(entry.directory) +
        L"|" + std::to_wstring(entry.size) + L"|" +
        std::to_wstring(entry.allocated_size);
    for (const MftTree::Name& name : entry.names) {
      line += L"|" + name.name + L"@" + std::to_wstring(name.parent_record) +
              std::to_wstring(name.parent_valid) +
              std::to_wstring(name.dos_only);
    }
    lines.push_back(line);
  }
  const MftTree::ScanStats& stats = tree.Stats();
  lines.push_back(std::to_wstring(stats.slots) + L"," +
                  std::to_wstring(stats.in_use) + L"," +
                  std::to_wstring(stats.deleted) + L"," +
                  std::to_wstring(stats.extensions) + L"," +
                  std::to_wstring(stats.unreadable) + L"," +
                  std::to_wstring(stats.damaged) + L"," +
                  std::to_wstring(stats.unreachable));
  return lines;
}

}  // namespace

TEST_CASE(
    "MftTree over FullCache at a tiny ceiling matches every other "
    "ceiling and NoCache",
    "[cache][mft-tree]") {
  const auto no_cache = OpenTreeVolume<Cache::Strategy::NoCache>(roomy_ceiling);
  const std::vector<std::wstring> expected = Describe(MftTree(*no_cache));
  REQUIRE(expected.size() > 1);

  const auto tiny =
      OpenTreeVolume<Cache::Strategy::FullCache>(one_block_ceiling);
  const auto zero = OpenTreeVolume<Cache::Strategy::FullCache>(0);
  const auto roomy = OpenTreeVolume<Cache::Strategy::FullCache>(roomy_ceiling);
  const auto unlimited =
      OpenTreeVolume<Cache::Strategy::FullCache>(Cache::unlimited_bytes);

  CHECK(Describe(MftTree(*tiny)) == expected);
  CHECK(Describe(MftTree(*zero)) == expected);
  CHECK(Describe(MftTree(*roomy)) == expected);
  CHECK(Describe(MftTree(*unlimited)) == expected);
  CHECK(tiny->GetCacheStats().held_bytes <= one_block_ceiling);
  CHECK(zero->GetCacheStats().held_bytes == 0);
}
