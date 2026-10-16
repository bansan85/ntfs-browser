#include "cache/block-cache.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Cache {

namespace {

// a + b, stopped at the largest value instead of wrapping. The totals are
// compared with the ceiling, so a wrapped one would look tiny.
constexpr std::uint64_t SaturatingAdd(std::uint64_t a,
                                      std::uint64_t b) noexcept {
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  return b > max - a ? max : a + b;
}

}  // namespace

Block::Block(std::size_t size)
    : bytes(std::make_unique_for_overwrite<BYTE[]>(size)), size(size) {}

StitchedBuffer::StitchedBuffer(std::size_t size,
                               std::shared_ptr<std::uint64_t> tally)
    : bytes(std::make_unique_for_overwrite<BYTE[]>(size)),
      size(size),
      tally(std::move(tally)) {
  // Last statement: nothing after the allocation can throw, so a buffer that
  // is counted is always destroyed, and uncounted.
  *this->tally = SaturatingAdd(*this->tally, size);
}

StitchedBuffer::~StitchedBuffer() {
  *tally = *tally >= size ? *tally - size : 0;
}

BlockCache::BlockCache(std::size_t block_bytes, std::uint64_t ceiling)
    : block_bytes_(block_bytes),
      ceiling_(ceiling),
      stitched_bytes_(std::make_shared<std::uint64_t>(0)) {}

std::shared_ptr<Block> BlockCache::Find(std::uint64_t index) {
  const auto found = blocks_.find(index);
  if (found == blocks_.end()) {
    return nullptr;
  }
  order_.splice(order_.end(), order_, found->second.position);
  return found->second.block;
}

void BlockCache::Insert(std::uint64_t index, std::shared_ptr<Block> block) {
  if (blocks_.contains(index)) {
    return;
  }
  order_.push_back(index);
  try {
    blocks_.emplace(index, Entry{std::move(block), std::prev(order_.end())});
  } catch (...) {
    order_.pop_back();
    throw;
  }
}

BlockCache::Stitched BlockCache::NewStitched(std::size_t size) {
  auto buffer = std::make_shared<StitchedBuffer>(size, stitched_bytes_);
  const std::span<BYTE> bytes{buffer->bytes.get(), size};
  return {.owner = std::move(buffer), .bytes = bytes};
}

void BlockCache::Trim() noexcept {
  auto next = order_.begin();
  while (next != order_.end() && HeldBytes() > ceiling_) {
    const auto found = blocks_.find(*next);
    // A second holder is a View: that block stays.
    if (found->second.block.use_count() > 1) {
      ++next;
      continue;
    }
    blocks_.erase(found);
    next = order_.erase(next);
  }
}

void BlockCache::Minify() noexcept {
  auto next = order_.begin();
  while (next != order_.end()) {
    const auto found = blocks_.find(*next);
    if (found->second.block.use_count() > 1) {
      ++next;
      continue;
    }
    blocks_.erase(found);
    next = order_.erase(next);
  }
}

std::uint64_t BlockCache::HeldBytes() const noexcept {
  const std::uint64_t cached =
      static_cast<std::uint64_t>(block_bytes_) * blocks_.size();
  return SaturatingAdd(cached, *stitched_bytes_);
}

std::uint64_t BlockCache::InUseBytes() const noexcept {
  std::uint64_t in_use = *stitched_bytes_;
  for (const auto& [index, entry] : blocks_) {
    if (entry.block.use_count() > 1) {
      in_use = SaturatingAdd(in_use, block_bytes_);
    }
  }
  return in_use;
}

Stats BlockCache::GetStats() const noexcept {
  return {.held_bytes = HeldBytes(), .in_use_bytes = InUseBytes()};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
