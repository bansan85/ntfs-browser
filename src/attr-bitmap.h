#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "internal-export.h"
#include "ntfs-common.h"

namespace NtfsBrowser {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
template <Cache::Strategy S>
class FileRecord;

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrBitmap : public Resident {
 public:
  AttrBitmap(const HeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(const AttrBitmap& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(const AttrBitmap& other) = delete;

  ~AttrBitmap() override { Log::Trace("AttrBitmap deleted"); }

 private:
  ULONGLONG bitmap_size_;         // Bitmap data size
  std::vector<BYTE> bitmap_buf_;  // Bitmap data buffer
  std::optional<ULONGLONG> current_cluster_;

 public:
  // Verify if a single cluster is free
  [[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
      IsClusterFree(ULONGLONG cluster);

};  // AttrBitmap

}  // namespace Attr

}  // namespace NtfsBrowser
