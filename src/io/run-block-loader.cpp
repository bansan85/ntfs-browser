#include "io/run-block-loader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "cache/block-cache.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

RunBlockLoader::RunBlockLoader(Cache::BlockCache& cache, const Limits& limits,
                               ReadFn read)
    : cache_(cache),
      block_bytes_(limits.block_bytes),
      max_run_blocks_(static_cast<std::uint64_t>(limits.max_run_bytes /
                                                 limits.block_bytes)),
      last_index_(limits.last_index),
      read_(std::move(read)) {}

std::shared_ptr<Cache::Block> RunBlockLoader::Get(LARGE_INTEGER block_addr) {
  const auto index =
      static_cast<std::uint64_t>(block_addr.QuadPart / block_bytes_);
  if (std::shared_ptr<Cache::Block> hit = cache_.Find(index)) {
    return hit;
  }
  const bool in_run =
      run_count_ != 0 && index >= run_first_ && index - run_first_ < run_count_;
  if (!in_run && !ReadRun(index, block_addr)) {
    return cache_.GetOrLoad(
        index, [&](std::span<BYTE> dest) { return read_(block_addr, dest); });
  }
  auto block =
      std::make_shared<Cache::Block>(static_cast<size_t>(block_bytes_));
  const std::span<const BYTE> source{
      run_bytes_.get(), static_cast<size_t>(run_count_ * block_bytes_)};
  const std::span<const BYTE> part =
      source.subspan(static_cast<size_t>((index - run_first_) * block_bytes_),
                     static_cast<size_t>(block_bytes_));
  std::ranges::copy(part, block->bytes.get());
  cache_.Insert(index, block);
  if (index - run_first_ + 1 == run_count_) {
    run_count_ = 0;
  }
  return block;
}

bool RunBlockLoader::ReadRun(std::uint64_t index, LARGE_INTEGER block_addr) {
  std::uint64_t count = 1;
  while (count < max_run_blocks_ && index + count <= last_index_ &&
         !cache_.Contains(index + count)) {
    ++count;
  }
  if (count == 1 || failed_) {
    return false;
  }
  const auto run_size = static_cast<size_t>(count * block_bytes_);
  if (run_capacity_ < run_size) {
    // Grown only when this run is longer than any before it in this read.
    run_capacity_ = run_size;
    run_bytes_ = std::make_unique_for_overwrite<BYTE[]>(run_size);
  }
  if (!read_(block_addr, {run_bytes_.get(), run_size})) {
    failed_ = true;
    return false;
  }
  run_first_ = index;
  run_count_ = count;
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
