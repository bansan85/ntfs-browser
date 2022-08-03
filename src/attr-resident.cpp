#include <crtdbg.h>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrBase(ahc, fr), AttrHeaderR((const Attr::HeaderResident&)ahc)
{
  AttrBody = (void*)((BYTE*)&AttrHeaderR + AttrHeaderR.AttrOffset);
  AttrBodySize = AttrHeaderR.AttrSize;
}

BOOL AttrResident::IsDataRunOK() const
{
  return TRUE;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize() const { return (ULONGLONG)AttrBodySize; }

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
BOOL AttrResident::ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                            DWORD* actural) const
{
  _ASSERT(bufv);

  *actural = 0;
  if (bufLen == 0) return TRUE;

  DWORD offsetd = (DWORD)offset;
  if (offsetd >= AttrBodySize) return FALSE;  // offset parameter error

  if ((offsetd + bufLen) > AttrBodySize)
    *actural = AttrBodySize - offsetd;  // Beyond scope
  else
    *actural = bufLen;

  memcpy(bufv, (BYTE*)AttrBody + offsetd, *actural);

  return TRUE;
}

}  // namespace NtfsBrowser