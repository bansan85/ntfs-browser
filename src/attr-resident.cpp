#include <crtdbg.h>

#include <gsl/narrow>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

AttrResident::AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrBase(ahc, fr)
{
}

bool AttrResident::IsDataRunOK() const noexcept
{
  return true;  // Always OK for a resident attribute
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
std::optional<ULONGLONG>
    AttrResident::ReadData(ULONGLONG offset,
                           const std::span<BYTE>& buffer) const
{
  ULONGLONG bufLen = buffer.size();
  ULONGLONG actural = 0;
  if (bufLen == 0)
  {
    return bufLen;
  }

  // offset parameter error
  if (offset >= GetDataSize())
  {
    return {};
  }

  if ((offset + bufLen) > GetDataSize())
  {
    actural = gsl::narrow<DWORD>(GetDataSize() - offset);  // Beyond scope
  }
  else
  {
    actural = bufLen;
  }

  memcpy(buffer.data(), &GetData()[offset], actural);

  return bufLen;
}

AttrResidentLight::AttrResidentLight(const AttrHeaderCommon& ahc,
                                     const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);

  body_ = std::span<const BYTE>{
      &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
      header.attr_size};
}

const BYTE* AttrResidentLight::GetData() const noexcept { return body_.data(); }

ULONGLONG AttrResidentLight::GetDataSize() const noexcept
{
  return body_.size();
}

AttrResidentHeavy::AttrResidentHeavy(const AttrHeaderCommon& ahc,
                                     const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);

  body_.resize(header.attr_size);
  memcpy(body_.data(),
         &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
         header.attr_size);
}

const BYTE* AttrResidentHeavy::GetData() const noexcept { return body_.data(); }

ULONGLONG AttrResidentHeavy::GetDataSize() const noexcept
{
  return body_.size();
}

}  // namespace NtfsBrowser