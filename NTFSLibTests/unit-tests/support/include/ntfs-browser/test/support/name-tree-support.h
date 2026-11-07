#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE {
namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Test::DiskReaders {

class MemoryDiskReader;

}  // namespace Test::DiskReaders
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests {

// The MFT record of node of BuildFakeNtfsImageWithNameTree().
[[nodiscard]] inline ULONGLONG NameTreeRecord(std::size_t node) {
  return Test::FakeImage::name_tree_first_record + node;
}

// Opens BuildFakeNtfsImageWithNameTree(nodes) over a reader in memory.
template <Cache::Strategy S>
[[nodiscard]] std::unique_ptr<NtfsVolume<S>> OpenNameTreeVolume(
    std::span<const Test::FakeImage::FakeTreeNode> nodes,
    const VolumeOptions& options = {},
    const Test::FakeImage::FakeNameTreeOptions& tree_options = {}) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(
          Test::FakeImage::BuildFakeNtfsImageWithNameTree(nodes, tree_options)),
      options);
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
