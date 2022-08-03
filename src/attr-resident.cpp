#include <crtdbg.h>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrBase(ahc, fr), attr_header_r_((const Attr::HeaderResident&)ahc)
{
  attr_body_ = (void*)((BYTE*)&attr_header_r_ + attr_header_r_.AttrOffset);
  attr_body_size_ = attr_header_r_.AttrSize;
}

BOOL AttrResident::IsDataRunOK() const
{
  return TRUE;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize() const
{
  return (ULONGLONG)attr_body_size_;
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
BOOL AttrResident::ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                            DWORD* actural) const
{
  _ASSERT(bufv);

  *actural = 0;
  if (bufLen == 0) return TRUE;

  DWORD offsetd = (DWORD)offset;
  if (offsetd >= attr_body_size_) return FALSE;  // offset parameter error

  if ((offsetd + bufLen) > attr_body_size_)
    *actural = attr_body_size_ - offsetd;  // Beyond scope
  else
    *actural = bufLen;

  memcpy(bufv, (BYTE*)attr_body_ + offsetd, *actural);

  return TRUE;
}

}  // namespace NtfsBrowser