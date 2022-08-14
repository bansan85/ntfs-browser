#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

template <class TYPE_RESIDENT>
class AttrBitmap : public TYPE_RESIDENT
{
 public:
  AttrBitmap(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(AttrBitmap const& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(AttrBitmap const& other) = delete;
  ~AttrBitmap() override { NTFS_TRACE("AttrBitmap deleted\n"); }

 private:
  ULONGLONG bitmap_size_;         // Bitmap data size
  std::vector<BYTE> bitmap_buf_;  // Bitmap data buffer
  std::optional<ULONGLONG> current_cluster_{};

 public:
  // Verify if a single cluster is free
  [[nodiscard]] bool IsClusterFree(ULONGLONG cluster);

};  // AttrBitmap

}  // namespace NtfsBrowser