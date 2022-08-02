#pragma once

#include <vector>

#include <ntfs-browser/index-entry.h>

namespace NtfsBrowser
{
namespace Data
{
struct IndexBlock;
}  // namespace Data

///////////////////////////////
// Index Block helper class
///////////////////////////////
class IndexBlock : public std::vector<IndexEntry*>
{
 public:
  IndexBlock();

  virtual ~IndexBlock();

 private:
  Data::IndexBlock* index_block_;

 public:
  Data::IndexBlock* AllocIndexBlock(DWORD size);
};  // IndexBlock

}  // namespace NtfsBrowser