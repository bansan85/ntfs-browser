#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

AttrBase::AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr) noexcept
    : attr_header_(ahc),
      file_record_(fr),
      sector_size_(fr.volume_.sector_size_),
      cluster_size_(fr.volume_.cluster_size_),
      index_block_size_(fr.volume_.index_block_size_),
      hvolume_(fr.volume_.hvolume_)
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
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
std::wstring AttrBase::GetAttrName() const
{
  if (attr_header_.name_length == 0)
  {
    NTFS_TRACE("Attribute is unnamed\n");
    return {};
  }
  std::wstring retval;
  retval.resize(attr_header_.name_length, '\0');
  const auto* namePtr = reinterpret_cast<const wchar_t*>(
      reinterpret_cast<const BYTE*>(&attr_header_) + attr_header_.name_offset);
  retval.assign(namePtr, attr_header_.name_length);

  NTFS_TRACE("Unicode Attribute Name\n");
  return retval;
}

// Verify if this attribute is unnamed
// Useful in analyzing MultiStream files
bool AttrBase::IsUnNamed() const noexcept
{
  return attr_header_.name_length == 0;
}

WORD AttrBase::GetSectorSize() const noexcept { return sector_size_; }
DWORD AttrBase::GetClusterSize() const noexcept { return cluster_size_; }
DWORD AttrBase::GetIndexBlockSize() const noexcept { return index_block_size_; }
HANDLE AttrBase::GetHandle() const noexcept { return hvolume_; }

}  // namespace NtfsBrowser
