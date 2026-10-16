#pragma once

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace {

// Bytes of one FullCache block.
constexpr std::uint64_t walk_cache_block = 65536;

// Blocks of index data the walk image holds, one cluster each: 2 MiB, so 32
// cache blocks. Well past the small ceilings the tests set.
constexpr DWORD walk_index_blocks = 2048;

// The soft ceiling of the "tiny" walks: four cache blocks.
constexpr std::uint64_t walk_tiny_ceiling = 4 * walk_cache_block;

// recover_errors makes a directory walk read every index block of the
// allocation, not only the ones the B+ tree reaches.
[[nodiscard]] inline NTFS_BROWSER_NAMESPACE::VolumeOptions
    WalkOptions(std::uint64_t max_cache_bytes) {
  return {.recover_errors = true, .max_cache_bytes = max_cache_bytes};
}

// A volume over the walk image.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] std::unique_ptr<NTFS_BROWSER_NAMESPACE::NtfsVolume<S>>
    OpenWalkVolume(const std::vector<BYTE>& image,
                   const NTFS_BROWSER_NAMESPACE::VolumeOptions& options) {
  auto volume = std::make_unique<NTFS_BROWSER_NAMESPACE::NtfsVolume<S>>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(image),
      options);
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// The names of every entry the root directory lists, sorted. Every view the
// walk held is gone when this returns.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] std::vector<std::wstring>
    WalkRoot(const NTFS_BROWSER_NAMESPACE::NtfsVolume<S>& volume) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const NTFS_BROWSER_NAMESPACE::IndexEntryView& entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            entry.GetFilename());
      },
      &names);
  std::ranges::sort(names);
  return names;
}

// The names WalkRoot() returns on the walk image.
[[nodiscard]] inline std::vector<std::wstring> ExpectedWalkNames() {
  std::vector<std::wstring> names;
  names.reserve(walk_index_blocks);
  for (DWORD vcn = 0; vcn < walk_index_blocks; vcn++) {
    names.push_back(NTFS_BROWSER_NAMESPACE::Tests::ManyIndexBlocksName(vcn));
  }
  std::ranges::sort(names);
  return names;
}

}  // namespace
