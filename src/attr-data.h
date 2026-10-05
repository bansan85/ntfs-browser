#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

template <class Resident, Strategy S>
class AttrData : public Resident
{
 public:
  AttrData(const AttrHeaderCommon& ahc, const FileRecord<S>& file_record)
      : Resident(ahc, file_record)
  {
    LogTrace("Attribute: Data ({}Resident)",
             this->IsNonResident() ? "Non" : "");
  }
  AttrData(AttrData&& other) noexcept = delete;
  AttrData(AttrData const& other) = delete;
  AttrData& operator=(AttrData&& other) noexcept = delete;
  AttrData& operator=(AttrData const& other) = delete;

  ~AttrData() override { LogTrace("AttrData deleted"); }
};  // AttrData

}  // namespace NtfsBrowser