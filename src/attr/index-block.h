#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Attr {

// The entries are views into bytes_ and realigned_, which this object owns:
// they MUST NOT outlive it.
class IndexBlock : public std::vector<IndexEntryView> {
 public:
  IndexBlock() noexcept;
  IndexBlock(IndexBlock&& other) noexcept = delete;
  IndexBlock(const IndexBlock& other) = delete;
  IndexBlock& operator=(IndexBlock&& other) noexcept = delete;
  IndexBlock& operator=(const IndexBlock& other) = delete;
  virtual ~IndexBlock() = default;

  // Drops the previous content and returns a zeroed buffer of `size` bytes.
  [[nodiscard]] std::span<BYTE> AllocIndexBlock(DWORD size);

  // Where the aligned copies of the misaligned entries go. The entries view
  // them, so they stay valid as long as this block.
  [[nodiscard]] std::vector<std::vector<BYTE>>& RealignedStorage() noexcept;

  // The block as read, fixups removed. Empty until AllocIndexBlock().
  [[nodiscard]] std::span<const BYTE> Bytes() const noexcept;

  // Where the used part of the block ends: the offset of the first byte after
  // its end entry, from which the unused space (slack) runs to the end of the
  // block. Set only when every entry parsed up to the end entry without a
  // defect, so a damaged block never gives a slack.
  [[nodiscard]] std::optional<size_t> UsedEnd() const noexcept;
  void SetUsedEnd(size_t used_end) noexcept;

 private:
  // The block as read from disk.
  std::vector<BYTE> bytes_;
  // Aligned copies of the entries that sit at a misaligned address in bytes_.
  std::vector<std::vector<BYTE>> realigned_;
  std::optional<size_t> used_end_;
};  // IndexBlock

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
