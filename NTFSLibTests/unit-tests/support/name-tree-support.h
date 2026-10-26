#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// The MFT record of node of BuildFakeNtfsImageWithNameTree().
[[nodiscard]] inline ULONGLONG NameTreeRecord(std::size_t node) {
  return name_tree_first_record + node;
}

// Opens BuildFakeNtfsImageWithNameTree(nodes) over a reader in memory.
template <Cache::Strategy S>
[[nodiscard]] std::unique_ptr<NtfsVolume<S>>
    OpenNameTreeVolume(std::span<const FakeTreeNode> nodes,
                       const VolumeOptions& options = {},
                       const FakeNameTreeOptions& tree_options = {}) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(
          BuildFakeNtfsImageWithNameTree(nodes, tree_options)),
      options);
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
