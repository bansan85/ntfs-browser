#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "cache/block-cache.h"
#include "ntfs-browser/namespace.h"

namespace {

// Bytes of one block in the BlockCache tests. Small, so a test counts blocks
// without caring about the 64 KiB of the real reader.
constexpr std::size_t cache_test_block = 16;

// Loads block "index" with a fixed fill and returns it.
[[nodiscard]] inline std::shared_ptr<NTFS_BROWSER_NAMESPACE::Cache::Block>
    LoadTestBlock(NTFS_BROWSER_NAMESPACE::Cache::BlockCache& cache,
                  std::uint64_t index) {
  return cache.GetOrLoad(index, [](std::span<BYTE> dest) {
    for (BYTE& byte : dest) {
      byte = 0x11;
    }
    return true;
  });
}

}  // namespace
