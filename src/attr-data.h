#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{

/////////////////////////////////////
// Attribute: Data
/////////////////////////////////////
template <class TYPE_RESIDENT>
class AttrData : public TYPE_RESIDENT
{
 public:
  AttrData(const AttrHeaderCommon& ahc, const FileRecord& fr)
      : TYPE_RESIDENT(ahc, fr)
  {
    //NTFS_TRACE1("Attribute: Data (%sResident)\n", IsNonResident() ? "Non" : "");
  }

  virtual ~AttrData()
  { /*
    NTFS_TRACE("AttrData deleted\n");*/
  }
};  // AttrData

}  // namespace NtfsBrowser