#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <vector>

#include "ntfs-common.h"

namespace NtfsBrowser
{
enum class Strategy;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <class TYPE_RESIDENT, Strategy S>
class AttrBitmap : public TYPE_RESIDENT
{
 public:
  AttrBitmap(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(AttrBitmap const& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(AttrBitmap const& other) = delete;
  ~AttrBitmap() override { LogTrace("AttrBitmap deleted"); }

 private:
  ULONGLONG bitmap_size_;         // Bitmap data size
  std::vector<BYTE> bitmap_buf_;  // Bitmap data buffer
  std::optional<ULONGLONG> current_cluster_{};

 public:
  // Verify if a single cluster is free
  [[nodiscard]] bool IsClusterFree(ULONGLONG cluster);

};  // AttrBitmap

}  // namespace NtfsBrowser