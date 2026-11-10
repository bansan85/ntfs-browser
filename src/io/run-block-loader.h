#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

class BlockCache;
struct Block;

}  // namespace Cache

namespace Io {

// Loads the blocks of one FullCache read. A block that the cache lacks comes
// in with the missing blocks after it, up to the end of the range, in one disk
// read. The run is cut into blocks as the copy reaches them, so a small
// ceiling never evicts a block before its use. One loader serves one read: it
// MUST NOT outlive the cache. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY RunBlockLoader {
 public:
  // Reads dest.size() bytes at addr, like FileReader::ReadFromReader().
  using ReadFn = std::function<bool(LARGE_INTEGER& addr, std::span<BYTE> dest)>;

  // The sizes that shape the runs of one read.
  struct Limits {
    // Bytes of one block of the cache. At least 1.
    std::int64_t block_bytes;
    // Bytes one disk read covers at most. A run is never shorter than one
    // block, whatever this is.
    std::int64_t max_run_bytes;
    // Index of the last block the read needs: a run stops there.
    std::uint64_t last_index;
  };

  // cache MUST outlive the loader.
  RunBlockLoader(Cache::BlockCache& cache, const Limits& limits, ReadFn read);

  // Loads the block at block_addr, or returns the cached one. Null when the
  // disk read fails or comes back short: nothing is cached then.
  [[nodiscard]] std::shared_ptr<Cache::Block> Get(LARGE_INTEGER block_addr);

 private:
  // Reads the missing blocks from index on with one disk read, at most
  // max_run_blocks_ of them and none past the range. False when there is only
  // one, or when the read failed: the block is then read on its own.
  bool ReadRun(std::uint64_t index, LARGE_INTEGER block_addr);

  Cache::BlockCache& cache_;
  std::int64_t block_bytes_;
  std::uint64_t max_run_blocks_;
  std::uint64_t last_index_;
  ReadFn read_;
  std::uint64_t run_first_{0};
  std::uint64_t run_count_{0};
  // Kept from one run to the next of the same read, so that its pages are
  // faulted in once.
  std::unique_ptr<BYTE[]> run_bytes_;
  size_t run_capacity_{0};
  bool failed_{false};
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
