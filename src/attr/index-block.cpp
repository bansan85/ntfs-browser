#include "data/index-block.h"

#include <ntfs-browser/namespace.h>

#include "attr/index-block.h"
#include "log/ntfs-common.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

IndexBlock::IndexBlock() noexcept { Log::Trace("Index Block"); }

std::span<BYTE> IndexBlock::AllocIndexBlock(DWORD size) {
  clear();

  realigned_.clear();
  used_end_.reset();
  bytes_.assign(size, 0);

  return bytes_;
}

std::vector<std::vector<BYTE>>& IndexBlock::RealignedStorage() noexcept {
  return realigned_;
}

std::span<const BYTE> IndexBlock::Bytes() const noexcept { return bytes_; }

std::optional<size_t> IndexBlock::UsedEnd() const noexcept { return used_end_; }

void IndexBlock::SetUsedEnd(size_t used_end) noexcept { used_end_ = used_end; }

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
