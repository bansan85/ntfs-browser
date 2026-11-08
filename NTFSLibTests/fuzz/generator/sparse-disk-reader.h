#pragma once

#include <ntfs-browser/win-types.h>

#include <memory>
#include <span>
#include <string_view>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "sparse-image.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Serves a SparseImage. The image has no fixed size, as the replay reader of a
// saved trace has none.
class SparseDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // Serves image, which the reader shares.
  explicit SparseDiskReader(std::shared_ptr<const SparseImage> image);

  // Nothing can be opened: the image is in memory.
  bool Open(std::wstring_view path) override;

  // Reads every address: the bytes that no rule names are zero.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  std::shared_ptr<const SparseImage> image_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
