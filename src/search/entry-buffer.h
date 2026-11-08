#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Entries that a scan found and that nobody has taken yet, with the text they
// point at. Add() copies the name, the path and the cluster summary of an
// entry into memory that the buffer owns, so the views of the stored Entry
// stay valid until Clear(): the memory never moves while the buffer grows.
// Clear() keeps the memory for the next entries. Not thread safe: a scan fills
// one buffer while the caller reads another, and swaps them under its own lock.
class NTFS_BROWSER_EXPORT_TESTS_ONLY EntryBuffer {
 public:
  EntryBuffer() = default;
  EntryBuffer(const EntryBuffer&) = delete;
  EntryBuffer& operator=(const EntryBuffer&) = delete;
  EntryBuffer(EntryBuffer&&) = delete;
  EntryBuffer& operator=(EntryBuffer&&) = delete;
  ~EntryBuffer() = default;

  // Stores a copy of entry. The views and the cluster pointer of entry are
  // copied, so they MAY point at memory of the caller that dies after the call.
  void Add(const Entry& entry);

  // The stored entries, in the order of Add(). Valid until Clear() or Add().
  [[nodiscard]] std::span<const Entry> Entries() const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept;

  // Forgets every entry. Keeps the memory. A debug build overwrites the text
  // first with the unit 0xFFFE, to show a view that outlived its batch.
  void Clear() noexcept;

 private:
  // One block of text. A name never spans two.
  struct Chunk {
    std::unique_ptr<char16_t[]> units;
    std::size_t capacity{0};
    std::size_t used{0};
  };

  // Copies text into a chunk and returns the copy.
  std::u16string_view Store(std::u16string_view text);

  std::vector<Entry> entries_;
  std::vector<Chunk> chunks_;
  // The chunk that Store() fills now: the ones before it are full enough.
  std::size_t current_{0};
  // A deque never moves its elements, so the pointers of the entries hold.
  std::deque<ClusterSummary> summaries_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
