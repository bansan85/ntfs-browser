#include "attr-resident.h"

#include <cstring>
#include <span>
#include <stdexcept>

#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/strategy.h>

#include "attr/header-resident.h"
#include "ntfs-browser/win-types.h"

namespace NtfsBrowser
{
template <Strategy S>
class FileRecord;

namespace
{
// Returns the attribute body. ValidateResidentBounds() MUST have accepted
// the header first.
std::span<const BYTE> ResidentBody(const Attr::HeaderResident& header)
{
  return std::span<const BYTE>(reinterpret_cast<const BYTE*>(&header),
                               header.header.total_size)
      .subspan(header.attr_offset, header.attr_size);
}

// Rejects an attr_offset/attr_size pair reaching past the attribute's own
// total_size, which is already bounds-checked against the record buffer.
void ValidateResidentBounds(const Attr::HeaderResident& header)
{
  if (static_cast<ULONGLONG>(header.attr_offset) + header.attr_size >
      header.header.total_size)
  {
    throw std::runtime_error(
        "Resident attribute body exceeds attribute bounds.\n");
  }
}
}  // namespace

template <Strategy S>
AttrResident<S>::AttrResident(const AttrHeaderCommon& ahc,
                              const FileRecord<S>& file_record)
    : AttrBase<S>(ahc, file_record)
{
}

// A resident attribute's bytes live inline in the MFT record: NTFS still
// reserves space for them within the attribute record, padded to the
// record's own 8-byte alignment, which can exceed the real data size by a
// few bytes. This is what Windows itself reports as a resident attribute's
// allocation size, so match it instead of returning GetDataSize().
template <Strategy S>
ULONGLONG AttrResident<S>::GetAllocatedSize() const noexcept
{
  const auto& header =
      reinterpret_cast<const Attr::HeaderResident&>(this->GetAttrHeader());
  return header.header.total_size - header.attr_offset;
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

  const std::span<const BYTE> body(this->GetData(), this->GetDataSize());
  // offset < GetDataSize() was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  memcpy(buffer.data(), &body[offset], actural);

  return actural;
}

AttrResidentNoCache::AttrResidentNoCache(
    const AttrHeaderCommon& ahc,
    const FileRecord<Strategy::NO_CACHE>& file_record)
    : AttrResident(ahc, file_record)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);
  ValidateResidentBounds(header);

  body_ = ResidentBody(header);
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
    const AttrHeaderCommon& ahc,
    const FileRecord<Strategy::FULL_CACHE>& file_record)
    : AttrResident(ahc, file_record)
{
  const auto& header = reinterpret_cast<const Attr::HeaderResident&>(ahc);
  ValidateResidentBounds(header);

  body_.resize(header.attr_size);
  // An empty body has a null data(), which memcpy must not receive.
  if (header.attr_size != 0)
  {
    memcpy(body_.data(), ResidentBody(header).data(), header.attr_size);
  }
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