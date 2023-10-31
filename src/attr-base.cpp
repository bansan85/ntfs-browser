#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

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
    NTFS_TRACE("Attribute is unnamed\n");
    return {};
  }

  std::wstring_view retval{reinterpret_cast<const wchar_t*>(
                               reinterpret_cast<const BYTE*>(&attr_header_) +
                               attr_header_.name_offset),
                           attr_header_.name_length};

  NTFS_TRACE("Unicode Attribute Name\n");
  return retval;
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
  return volume_.sector_size_;
}

template <Strategy S>
DWORD AttrBase<S>::GetClusterSize() const noexcept
{
  return volume_.cluster_size_;
}

template <Strategy S>
DWORD AttrBase<S>::GetIndexBlockSize() const noexcept
{
  return volume_.index_block_size_;
}

template class AttrBase<Strategy::NO_CACHE>;
template class AttrBase<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser
