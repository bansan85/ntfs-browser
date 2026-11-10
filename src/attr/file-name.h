#pragma once

#include <ntfs-browser/namespace.h>

#include "attr/forward.h"
#include "filename.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrFileName : public Resident, public NTFS_BROWSER_NAMESPACE::Filename {
 public:
  AttrFileName(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(const AttrFileName& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(const AttrFileName& other) = delete;
  ~AttrFileName() override;
};  // AttrFileName

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
