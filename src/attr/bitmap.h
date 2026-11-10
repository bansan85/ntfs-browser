#pragma once

#include <ntfs-browser/namespace.h>

#include "attr/forward.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {

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
