#include "index-block.h"

#include "data/index-entry.h"
#include "data/index-block.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

IndexBlock::IndexBlock()
{
  NTFS_TRACE("Index Block\n");

  index_block_ = nullptr;
}

Data::IndexBlock* IndexBlock::AllocIndexBlock(DWORD size)
{
  // Free previous data if any
  if (this->size() > 0) clear();
  if (index_block_) delete index_block_;

  index_block_ = (Data::IndexBlock*)new BYTE[size];

  return index_block_;
}

}  // namespace NtfsBrowser