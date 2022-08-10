#include "data/index-block.h"
#include "data/index-entry.h"
#include "index-block.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

IndexBlock::IndexBlock() noexcept { NTFS_TRACE("Index Block\n"); }

Data::IndexBlock* IndexBlock::AllocIndexBlock(DWORD size)
{
  // Free previous data if any
  if (!this->empty())
  {
    clear();
  }

  index_block_.resize(size);

  return reinterpret_cast<Data::IndexBlock*>(index_block_.data());
}

}  // namespace NtfsBrowser