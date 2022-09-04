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

  memcpy(body_.data(),
         &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
         body_.size());
}

bool AttrResident::IsDataRunOK() const noexcept
{
  return true;  // Always OK for a resident attribute
}

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrResident::GetDataSize() const noexcept { return body_.size(); }

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
std::optional<ULONGLONG> AttrResident::ReadData(ULONGLONG offset,
                                                std::span<BYTE> buffer) const
{
  ULONGLONG bufLen = buffer.size();
  ULONGLONG actural = 0;
  if (bufLen == 0)
  {
    return bufLen;
  }

  // offset parameter error
  if (offset >= body_.size())
  {
    return {};
  }

  if ((offset + bufLen) > body_.size())
  {
    actural = gsl::narrow<DWORD>(body_.size() - offset);  // Beyond scope
  }
  else
  {
    actural = bufLen;
  }

  memcpy(buffer.data(), &body_[offset], actural);

  return bufLen;
}

const BYTE* AttrResident::GetData() const noexcept { return body_.data(); }

}  // namespace NtfsBrowser