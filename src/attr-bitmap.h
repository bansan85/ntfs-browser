#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "internal-export.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <class Resident, Strategy S>
class AttrBitmap : public Resident
{
 public:
  AttrBitmap(const AttrHeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(AttrBitmap const& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(AttrBitmap const& other) = delete;
  ~AttrBitmap() override { LogTrace("AttrBitmap deleted"); }

 private:
  ULONGLONG bitmap_size_;         // Bitmap data size
  std::vector<BYTE> bitmap_buf_;  // Bitmap data buffer
  std::optional<ULONGLONG> current_cluster_;

 public:
  // Verify if a single cluster is free
  [[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
      IsClusterFree(ULONGLONG cluster);

};  // AttrBitmap

}  // namespace NtfsBrowser