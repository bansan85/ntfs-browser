#include <ntfs-browser/win-types.h>

#include <string>
#include <string_view>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>  // IWYU pragma: keep
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "ntfs-common.h"
#include "utf.h"

namespace NtfsBrowser
{
enum class AttrType : DWORD;

template <Strategy S>
AttrBase<S>::AttrBase(const AttrHeaderCommon& ahc,
                      const FileRecord<S>& fr) noexcept
    : attr_header_(ahc), volume_(fr.GetVolume())
{
}

template <Strategy S>
const AttrHeaderCommon& AttrBase<S>::GetAttrHeader() const noexcept
{
  return attr_header_;
}

template <Strategy S>
AttrType AttrBase<S>::GetAttrType() const noexcept
{
  return attr_header_.type;
}

template <Strategy S>
DWORD AttrBase<S>::GetAttrTotalSize() const noexcept
{
  return attr_header_.total_size;
}

template <Strategy S>
bool AttrBase<S>::IsNonResident() const noexcept
{
  return attr_header_.non_resident != 0;
}

template <Strategy S>
WORD AttrBase<S>::GetAttrFlags() const noexcept
{
  return attr_header_.flags;
}

// Get UNICODE Attribute name
template <Strategy S>
std::wstring_view AttrBase<S>::GetAttrName() const
{
  if (attr_header_.name_length == 0)
  {
    LogTrace("Attribute is unnamed");
    return {};
  }

  if (static_cast<ULONGLONG>(attr_header_.name_offset) +
          (static_cast<ULONGLONG>(attr_header_.name_length) * sizeof(WCHAR)) >
      attr_header_.total_size)
  {
    LogWarn("Attribute name exceeds attribute bounds.");
    return {};
  }

  // The name sits at name_offset bytes past the header, as raw on-disk
  // UTF-16 (WCHAR, always 16 bits) - not wchar_t, wider than that off
  // Windows, so this decodes rather than reinterpret_casts.
  attr_name_cache_ = Utf16ToWide(
      std::u16string_view(reinterpret_cast<const char16_t*>(
                              reinterpret_cast<const BYTE*>(&attr_header_) +
                              attr_header_.name_offset),
                          attr_header_.name_length));

  LogTrace("Unicode Attribute Name");
  return attr_name_cache_;
}

// Verify if this attribute is unnamed
// Useful in analyzing MultiStream files
template <Strategy S>
bool AttrBase<S>::IsUnNamed() const noexcept
{
  return attr_header_.name_length == 0;
}

template <Strategy S>
WORD AttrBase<S>::GetSectorSize() const noexcept
{
  return volume_.GetSectorSize();
}

template <Strategy S>
DWORD AttrBase<S>::GetClusterSize() const noexcept
{
  return volume_.GetClusterSize();
}

template <Strategy S>
DWORD AttrBase<S>::GetIndexBlockSize() const noexcept
{
  return volume_.GetIndexBlockSize();
}

template class AttrBase<Strategy::NO_CACHE>;
template class AttrBase<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser
