#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "ntfs-common.h"

namespace NtfsBrowser {

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrData : public Resident {
 public:
  AttrData(const HeaderCommon& ahc, const FileRecord<S>& file_record)
      : Resident(ahc, file_record) {
    Log::Trace("Attribute: Data ({}Resident)",
               this->IsNonResident() ? "Non" : "");
  }

  AttrData(AttrData&& other) noexcept = delete;
  AttrData(const AttrData& other) = delete;
  AttrData& operator=(AttrData&& other) noexcept = delete;
  AttrData& operator=(const AttrData& other) = delete;

  ~AttrData() override { Log::Trace("AttrData deleted"); }
};  // AttrData

}  // namespace Attr

}  // namespace NtfsBrowser
