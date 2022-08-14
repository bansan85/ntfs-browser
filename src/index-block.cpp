#include "data/index-block.h"
#include "data/index-entry.h"
#include "index-block.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

IndexBlock::IndexBlock() noexcept { NTFS_TRACE("Index Block\n"); }

std::shared_ptr<BYTE[]> IndexBlock::AllocIndexBlock(DWORD size)
{
  clear();

  index_block_ = std::make_shared<BYTE[]>(size);

  return index_block_;
}

}  // namespace NtfsBrowser