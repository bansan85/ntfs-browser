#include <crtdbg.h>

#include <gsl/narrow>

#include "attr-resident.h"
#include "attr/header-resident.h"

namespace NtfsBrowser
{

template <Strategy S>
AttrResident<S>::AttrResident(const AttrHeaderCommon& ahc,
                              const FileRecord<S>& fr)
    : AttrBase<S>(ahc, fr)
{
}

template <Strategy S>
bool AttrResident<S>::IsDataRunOK() const noexcept
{
  return true;  // Always OK for a resident attribute
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
template <Strategy S>
std::optional<ULONGLONG>
    AttrResident<S>::ReadData(ULONGLONG offset,
                              const std::span<BYTE>& buffer) const
{
  ULONGLONG bufLen = buffer.size();
  ULONGLONG actural = 0;
  if (bufLen == 0)
  {
    return bufLen;
  }

  // offset parameter error
  if (offset >= this->GetDataSize())
  {
    return {};
  }

  if ((offset + bufLen) > this->GetDataSize())
  {
    actural = gsl::narrow<DWORD>(this->GetDataSize() - offset);  // Beyond scope
  }
  else
  {
    actural = bufLen;
  }

  memcpy(buffer.data(), &this->GetData()[offset], actural);

  return bufLen;
}

AttrResidentNoCache::AttrResidentNoCache(
    const AttrHeaderCommon& ahc, const FileRecord<Strategy::NO_CACHE>& fr)
    : AttrResident(ahc, fr)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);

  body_ = std::span<const BYTE>{
      &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
      header.attr_size};
}

const BYTE* AttrResidentNoCache::GetData() const noexcept
{
  return body_.data();
}

ULONGLONG AttrResidentNoCache::GetDataSize() const noexcept
{
  return body_.size();
}

AttrResidentFullCache::AttrResidentFullCache(
    const AttrHeaderCommon& ahc, const FileRecord<Strategy::FULL_CACHE>& fr)
    : AttrResident(ahc, fr)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);

  body_.resize(header.attr_size);
  memcpy(body_.data(),
         &reinterpret_cast<const BYTE*>(&header)[header.attr_offset],
         header.attr_size);
}

const BYTE* AttrResidentFullCache::GetData() const noexcept
{
  return body_.data();
}

ULONGLONG AttrResidentFullCache::GetDataSize() const noexcept
{
  return body_.size();
}

}  // namespace NtfsBrowser