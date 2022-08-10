#pragma once

#include <memory>
#include <vector>

#include <ntfs-browser/index-entry.h>

// OK

namespace NtfsBrowser
{
namespace Data
{
struct IndexBlock;
}  // namespace Data

///////////////////////////////
// Index Block helper class
///////////////////////////////
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
  std::vector<BYTE> index_block_;

 public:
  Data::IndexBlock* AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace NtfsBrowser