#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

template <class TYPE_RESIDENT, Strategy S>
class AttrData : public TYPE_RESIDENT
{
 public:
  AttrData(const AttrHeaderCommon& ahc, const FileRecord<S>& fr)
      : TYPE_RESIDENT(ahc, fr)
  {
    NTFS_TRACE1("Attribute: Data (%sResident)\n",
                this->IsNonResident() ? "Non" : "");
  }
  AttrData(AttrData&& other) noexcept = delete;
  AttrData(AttrData const& other) = delete;
  AttrData& operator=(AttrData&& other) noexcept = delete;
  AttrData& operator=(AttrData const& other) = delete;

  ~AttrData() override { NTFS_TRACE("AttrData deleted\n"); }
};  // AttrData

}  // namespace NtfsBrowser