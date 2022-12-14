#pragma once

#include <memory>
#include <vector>

#include <ntfs-browser/index-entry.h>

namespace NtfsBrowser
{
namespace Data
{
struct IndexBlock;
}  // namespace Data

class IndexBlock : public std::vector<IndexEntry>
{
 public:
  IndexBlock() noexcept;
  IndexBlock(IndexBlock&& other) noexcept = delete;
  IndexBlock(IndexBlock const& other) = delete;
  IndexBlock& operator=(IndexBlock&& other) noexcept = delete;
  IndexBlock& operator=(IndexBlock const& other) = delete;
  virtual ~IndexBlock() = default;

 private:
  std::shared_ptr<BYTE[]> index_block_;

 public:
  [[nodiscard]] std::shared_ptr<BYTE[]> AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace NtfsBrowser