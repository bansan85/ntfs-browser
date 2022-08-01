#pragma once

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/data/index-entry.h>
#include <ntfs-browser/data/index-block.h>

namespace NtfsBrowser
{

///////////////////////////////
// Index Block helper class
///////////////////////////////
class IndexBlock : public std::vector<IndexEntry*>
{
 public:
  IndexBlock()
  {
    NTFS_TRACE("Index Block\n");

    index_block_ = NULL;
  }

  virtual ~IndexBlock()
  {
    NTFS_TRACE("index_block_ deleted\n");

    if (index_block_) delete index_block_;
  }

 private:
  Data::IndexBlock* index_block_;

 public:
  Data::IndexBlock* AllocIndexBlock(DWORD size)
  {
    // Free previous data if any
    if (this->size() > 0) clear();
    if (index_block_) delete index_block_;

    index_block_ = (Data::IndexBlock*)new BYTE[size];

    return index_block_;
  }
};  // IndexBlock

}  // namespace NtfsBrowser