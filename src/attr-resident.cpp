#include <crtdbg.h>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrBase(ahc, fr), header_r_((const Attr::HeaderResident&)ahc)
{
  body_.resize(header_r_.AttrSize);
  memcpy(body_.data(), (BYTE*)&header_r_ + header_r_.AttrOffset, body_.size());
}

BOOL AttrResident::IsDataRunOK() const
{
  return TRUE;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize() const { return (ULONGLONG)body_.size(); }

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
BOOL AttrResident::ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                            DWORD* actural) const
{
  _ASSERT(bufv);

  *actural = 0;
  if (bufLen == 0) return TRUE;

  DWORD offsetd = (DWORD)offset;
  if (offsetd >= body_.size()) return FALSE;  // offset parameter error

  if ((offsetd + bufLen) > body_.size())
    *actural = body_.size() - offsetd;  // Beyond scope
  else
    *actural = bufLen;

  memcpy(bufv, (BYTE*)body_.data() + offsetd, *actural);

  return TRUE;
}

const BYTE* AttrResident::GetData() { return body_.data(); }

}  // namespace NtfsBrowser