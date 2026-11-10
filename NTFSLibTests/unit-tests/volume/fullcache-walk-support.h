#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "attr/mask.h"
#include "index-entry.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "volume-options.h"
#include "win-types.h"

namespace {

// Bytes of one FullCache block: the default of the tuning.
constexpr std::uint64_t walk_cache_block =
    NTFS_BROWSER_NAMESPACE::Tuning::Settings{}.cache.block_bytes;

// Blocks of index data the walk image holds, one cluster each: 2 MiB, so 512
// cache blocks of 4 KiB. Well past the small ceilings the tests set.
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
      std::make_unique<
          NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(image),
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
  root.SetParseOptions(
      {.attr_mask = NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                    NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation});
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
    names.push_back(
        NTFS_BROWSER_NAMESPACE::Test::FakeImage::ManyIndexBlocksName(vcn));
  }
  std::ranges::sort(names);
  return names;
}

}  // namespace
