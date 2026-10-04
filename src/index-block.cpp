#include "data/index-block.h"

#include "index-block.h"
#include "ntfs-browser/win-types.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

IndexBlock::IndexBlock() noexcept { LogTrace("Index Block"); }

std::shared_ptr<std::vector<BYTE>> IndexBlock::AllocIndexBlock(DWORD size)
{
  clear();

  index_block_ = std::make_shared<std::vector<BYTE>>(size);

  return index_block_;
}

}  // namespace NtfsBrowser