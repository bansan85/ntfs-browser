#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <ntfs-browser/index-entry.h>

namespace NtfsBrowser
{
enum class Strategy : std::uint8_t;

class IndexBlock : public std::vector<IndexEntry>
{
 public:
  IndexBlock() noexcept;
  IndexBlock(IndexBlock&& other) noexcept = delete;
  IndexBlock(IndexBlock const& other) = delete;
  IndexBlock& operator=(IndexBlock&& other) noexcept = delete;
  IndexBlock& operator=(IndexBlock const& other) = delete;
  virtual ~IndexBlock() = default;

  template <Strategy S>
  friend class AttrIndexAlloc;

 private:
  std::shared_ptr<std::vector<BYTE>> index_block_;

  [[nodiscard]] std::shared_ptr<std::vector<BYTE>> AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace NtfsBrowser