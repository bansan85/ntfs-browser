#pragma once

#include <memory>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
namespace Data
{
struct IndexBlock;
}  // namespace Data
template <Strategy S>
class AttrIndexAlloc;

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
  std::shared_ptr<BYTE[]> index_block_;

  [[nodiscard]] std::shared_ptr<BYTE[]> AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace NtfsBrowser