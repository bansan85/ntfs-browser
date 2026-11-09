#include "sparse-disk-reader.h"

#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "sparse-image.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

SparseDiskReader::SparseDiskReader(std::shared_ptr<const SparseImage> image)
    : image_(std::move(image)) {}

bool SparseDiskReader::Open(std::wstring_view /*path*/) { return false; }

bool SparseDiskReader::ReadInto(LARGE_INTEGER& addr,
                                std::span<BYTE> dest) const {
  if (addr.QuadPart < 0) {
    return false;
  }
  image_->Read(static_cast<ULONGLONG>(addr.QuadPart), dest);
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
