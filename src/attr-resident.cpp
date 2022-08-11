#include <crtdbg.h>

#include <gsl/narrow>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrBase(ahc, fr)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);
  body_.resize(header.attr_size);

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 26481)
  #pragma warning(disable : 26490)
#endif
  memcpy(body_.data(),
         // NOLINTNEXTLINE
         &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
         body_.size());
#ifdef _MSC_VER
  #pragma warning(pop)
#endif
}

bool AttrResident::IsDataRunOK() const noexcept
{
  return TRUE;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize() const noexcept { return body_.size(); }

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
bool AttrResident::ReadData(ULONGLONG offset, gsl::not_null<void*> bufv,
                            ULONGLONG bufLen, ULONGLONG& actural) const
{
  actural = 0;
  if (bufLen == 0)
  {
    return TRUE;
  }

  // offset parameter error
  if (offset >= body_.size())
  {
    return FALSE;
  }

  if ((offset + bufLen) > body_.size())
  {
    actural = gsl::narrow<DWORD>(body_.size() - offset);  // Beyond scope
  }
  else
  {
    actural = bufLen;
  }

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 26446)
#endif
  memcpy(bufv, &body_[offset], actural);
#ifdef _MSC_VER
  #pragma warning(pop)
#endif

  return TRUE;
}

const BYTE* AttrResident::GetData() const noexcept { return body_.data(); }

}  // namespace NtfsBrowser