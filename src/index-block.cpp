#include "data/index-block.h"

#include "index-block.h"
#include "ntfs-browser/win-types.h"
#include "ntfs-common.h"

namespace NtfsBrowser::Attr {

IndexBlock::IndexBlock() noexcept { Log::Trace("Index Block"); }

std::span<BYTE> IndexBlock::AllocIndexBlock(DWORD size) {
  clear();

  realigned_.clear();
  bytes_.assign(size, 0);

  return bytes_;
}

}  // namespace NtfsBrowser::Attr
