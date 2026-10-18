#pragma once

#include <ntfs-browser/win-types.h>

#include <span>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

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

 private:
  // The block as read from disk.
  std::vector<BYTE> bytes_;
  // Aligned copies of the entries that sit at a misaligned address in bytes_.
  std::vector<std::vector<BYTE>> realigned_;
};  // IndexBlock

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
