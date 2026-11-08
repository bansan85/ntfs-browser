#include "search/entry-buffer.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// Units of text in a new chunk: 64 KiB, a few thousand names, so a scan of a
// million files makes a few hundred allocations. A longer name gets a chunk of
// its own size.
constexpr std::size_t chunk_units = 32768;

#ifndef NDEBUG
// What Clear() writes over the text of the entries it forgets, in a debug
// build. A view that outlived its batch then shows this unit, which is no
// character (a Unicode noncharacter), so a test or a debugger sees the misuse.
constexpr char16_t poison_unit = 0xFFFE;
#endif

}  // namespace

void EntryBuffer::Add(const Entry& entry) {
  Entry stored = entry;
  stored.name = Store(entry.name);
  stored.path = Store(entry.path);
  if (entry.clusters != nullptr) {
    stored.clusters = &summaries_.emplace_back(*entry.clusters);
  }
  entries_.push_back(stored);
}

std::span<const Entry> EntryBuffer::Entries() const noexcept {
  return entries_;
}

std::size_t EntryBuffer::Size() const noexcept { return entries_.size(); }

void EntryBuffer::Clear() noexcept {
  entries_.clear();
  summaries_.clear();
  for (Chunk& chunk : chunks_) {
#ifndef NDEBUG
    std::fill_n(chunk.units.get(), chunk.used, poison_unit);
#endif
    chunk.used = 0;
  }
  current_ = 0;
}

std::u16string_view EntryBuffer::Store(std::u16string_view text) {
  if (text.empty()) {
    return {};
  }
  while (current_ < chunks_.size() &&
         chunks_[current_].capacity - chunks_[current_].used < text.size()) {
    ++current_;
  }
  if (current_ == chunks_.size()) {
    const std::size_t capacity = std::max(chunk_units, text.size());
    chunks_.push_back({.units = std::make_unique<char16_t[]>(capacity),
                       .capacity = capacity,
                       .used = 0});
  }
  Chunk& chunk = chunks_[current_];
  char16_t* const place = chunk.units.get() + chunk.used;
  std::ranges::copy(text, place);
  chunk.used += text.size();
  return {place, text.size()};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
