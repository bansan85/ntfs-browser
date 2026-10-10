#include "data/index-block.h"

#include "attr/index-block.h"
#include "log/ntfs-common.h"
#include "ntfs-browser/win-types.h"

namespace NtfsBrowser::Attr {

IndexBlock::IndexBlock() noexcept { Log::Trace("Index Block"); }

std::span<BYTE> IndexBlock::AllocIndexBlock(DWORD size) {
  clear();

  realigned_.clear();
  bytes_.assign(size, 0);

  return bytes_;
}

}  // namespace NtfsBrowser::Attr
