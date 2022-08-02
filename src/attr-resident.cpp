#include <crtdbg.h>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon* ahc, const FileRecord* fr)
    : AttrBase(ahc, fr)
{
  AttrHeaderR = (Attr::HeaderResident*)ahc;
  AttrBody = (void*)((BYTE*)AttrHeaderR + AttrHeaderR->AttrOffset);
  AttrBodySize = AttrHeaderR->AttrSize;
}

AttrResident::~AttrResident() {}

BOOL AttrResident::IsDataRunOK() const
{
  return TRUE;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize(ULONGLONG* allocSize) const
{
  if (allocSize) *allocSize = AttrBodySize;

  return (ULONGLONG)AttrBodySize;
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
BOOL AttrResident::ReadData(const ULONGLONG& offset, void* bufv, DWORD bufLen,
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