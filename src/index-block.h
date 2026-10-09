#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <span>
#include <vector>

#include <ntfs-browser/index-entry.h>

namespace NtfsBrowser {

enum class Strategy : std::uint8_t;

namespace Attr {

template <Strategy S>
class AttrIndexAlloc;

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

  template <Strategy S>
  friend class AttrIndexAlloc;

 private:
  // The block as read from disk.
  std::vector<BYTE> bytes_;
  // Aligned copies of the entries that sit at a misaligned address in bytes_.
  std::vector<std::vector<BYTE>> realigned_;

  // Drops the previous content and returns a zeroed buffer of `size` bytes.
  [[nodiscard]] std::span<BYTE> AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace Attr

}  // namespace NtfsBrowser
