#include "data/index-block.h"
#include "data/index-entry.h"
#include "index-block.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

IndexBlock::IndexBlock() noexcept { NTFS_TRACE("Index Block\n"); }

Data::IndexBlock* IndexBlock::AllocIndexBlock(DWORD size)
{
  clear();

  index_block_.resize(size);

  return reinterpret_cast<Data::IndexBlock*>(index_block_.data());
}

}  // namespace NtfsBrowser