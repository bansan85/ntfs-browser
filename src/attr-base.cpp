#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrBase::AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr) noexcept
    : attr_header_(ahc), volume_(fr.GetVolume())
{
}

const AttrHeaderCommon& AttrBase::GetAttrHeader() const noexcept
{
  return attr_header_;
}

DWORD AttrBase::GetAttrType() const noexcept { return attr_header_.type; }

DWORD AttrBase::GetAttrTotalSize() const noexcept
{
  return attr_header_.total_size;
}

bool AttrBase::IsNonResident() const noexcept
{
  return attr_header_.non_resident != 0;
}

WORD AttrBase::GetAttrFlags() const noexcept { return attr_header_.flags; }

// Get UNICODE Attribute name
std::wstring_view AttrBase::GetAttrName() const
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
bool AttrBase::IsUnNamed() const noexcept
{
  return attr_header_.name_length == 0;
}

WORD AttrBase::GetSectorSize() const noexcept { return volume_.sector_size_; }
DWORD AttrBase::GetClusterSize() const noexcept
{
  return volume_.cluster_size_;
}
DWORD AttrBase::GetIndexBlockSize() const noexcept
{
  return volume_.index_block_size_;
}

}  // namespace NtfsBrowser
