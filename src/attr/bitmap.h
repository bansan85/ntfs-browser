#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>

#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrBitmap : public Resident {
 public:
  AttrBitmap(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(const AttrBitmap& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(const AttrBitmap& other) = delete;

  ~AttrBitmap() override { Log::Trace("AttrBitmap deleted"); }
};  // AttrBitmap

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
