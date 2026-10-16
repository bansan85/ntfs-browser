#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <unordered_map>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Cache {

// One cached zone of the volume, read at once. One heap allocation of exactly
// the block size, so it can be freed on its own.
struct NTFS_BROWSER_EXPORT_TESTS_ONLY Block {
  explicit Block(std::size_t size);

  [[nodiscard]] std::span<BYTE> Bytes() noexcept { return {bytes.get(), size}; }

  std::unique_ptr<BYTE[]> bytes;
  std::size_t size;
};

// A buffer stitched from several blocks, or read around the cache. It belongs
// to the View that holds it, and its size counts in the cache's tally while it
// lives.
struct NTFS_BROWSER_EXPORT_TESTS_ONLY StitchedBuffer {
  StitchedBuffer(std::size_t size, std::shared_ptr<std::uint64_t> tally);
  StitchedBuffer(const StitchedBuffer&) = delete;
  StitchedBuffer& operator=(const StitchedBuffer&) = delete;
  ~StitchedBuffer();

  std::unique_ptr<BYTE[]> bytes;
  std::size_t size;
  std::shared_ptr<std::uint64_t> tally;
};

// Bounded LRU cache of fixed-size blocks, for Io::FileReader<FullCache>.
//
// The ceiling is soft. A block that a View still holds is never dropped, so
// the total can stay above it. Single-threaded, like the rest of the library.
// A BlockCache MUST be owned by a std::shared_ptr: views reach it weakly.
class NTFS_BROWSER_EXPORT_TESTS_ONLY BlockCache {
 public:
  // A stitched buffer, and the bytes it holds.
  struct Stitched {
    std::shared_ptr<const void> owner;
    std::span<BYTE> bytes;
  };

  // block_bytes is the size of every block. ceiling is the soft limit on
  // HeldBytes(); Cache::unlimited_bytes removes it.
  BlockCache(std::size_t block_bytes, std::uint64_t ceiling);

  // The block at index, or null. A hit counts as a use.
  [[nodiscard]] std::shared_ptr<Block> Find(std::uint64_t index);

  // Adds a block that Find() missed, as the most recently used. An index that
  // is already cached keeps its block, since a cached block never changes.
  void Insert(std::uint64_t index, std::shared_ptr<Block> block);

  // The block at index, loaded through fill on a miss. fill gets the whole
  // block and returns false when the read failed or came back short. Such a
  // block is never inserted, so no partial data stays in the cache. Returns
  // null on that failure. An exception from fill or from the allocation
  // reaches the caller, with the cache unchanged.
  template <typename Fill>
  [[nodiscard]] std::shared_ptr<Block> GetOrLoad(std::uint64_t index,
                                                 Fill&& fill) {
    if (std::shared_ptr<Block> hit = Find(index)) {
      return hit;
    }
    auto block = std::make_shared<Block>(block_bytes_);
    if (!fill(block->Bytes())) {
      return nullptr;
    }
    Insert(index, block);
    return block;
  }

  // A new buffer of size bytes for one View, counted in HeldBytes() and
  // InUseBytes() until its owner is dropped.
  [[nodiscard]] Stitched NewStitched(std::size_t size);

  // Drops unused blocks, least recently used first, until HeldBytes() is at
  // most the ceiling. Stops early when only blocks in use remain.
  void Trim() noexcept;

  // Drops every unused block.
  void Minify() noexcept;

  // Every block plus every live stitched buffer. The ceiling limits it.
  [[nodiscard]] std::uint64_t HeldBytes() const noexcept;

  // The part of HeldBytes() that a live View holds.
  [[nodiscard]] std::uint64_t InUseBytes() const noexcept;

  [[nodiscard]] Stats GetStats() const noexcept;

  [[nodiscard]] std::size_t BlockBytes() const noexcept { return block_bytes_; }

  // Bytes of live stitched buffers. Exposed so a test can set it near the
  // top of the range to check the totals saturate.
  [[nodiscard]] const std::shared_ptr<std::uint64_t>& StitchedTally() noexcept {
    return stitched_bytes_;
  }

 private:
  struct Entry {
    std::shared_ptr<Block> block;
    // Position in order_, kept to move the entry in O(1).
    std::list<std::uint64_t>::iterator position;
  };

  std::size_t block_bytes_;
  std::uint64_t ceiling_;
  std::unordered_map<std::uint64_t, Entry> blocks_;
  // Block indexes, least recently used first.
  std::list<std::uint64_t> order_;
  std::shared_ptr<std::uint64_t> stitched_bytes_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
