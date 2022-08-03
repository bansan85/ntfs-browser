#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrBase::AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : attr_header_(ahc), file_record_(fr)
{
  sector_size_ = fr.Volume.SectorSize;
  cluster_size_ = fr.Volume.ClusterSize;
  index_block_size_ = fr.Volume.IndexBlockSize;
  hvolume_ = fr.Volume.hVolume;
}

AttrBase::~AttrBase() {}

const AttrHeaderCommon& AttrBase::GetAttrHeader() const { return attr_header_; }

DWORD AttrBase::GetAttrType() const { return attr_header_.Type; }

DWORD AttrBase::GetAttrTotalSize() const { return attr_header_.TotalSize; }

BOOL AttrBase::IsNonResident() const { return attr_header_.NonResident; }

WORD AttrBase::GetAttrFlags() const { return attr_header_.Flags; }

// Get ANSI Attribute name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int AttrBase::GetAttrName(char* buf, DWORD bufLen) const
{
  if (attr_header_.NameLength)
  {
    if (bufLen < attr_header_.NameLength)
      return -1 * attr_header_.NameLength;  // buffer too small

    wchar_t* namePtr =
        (wchar_t*)((BYTE*)&attr_header_ + attr_header_.NameOffset);
    int len = WideCharToMultiByte(CP_ACP, 0, namePtr, attr_header_.NameLength,
                                  buf, bufLen, nullptr, nullptr);
    if (len)
    {
      buf[len] = '\0';
      NTFS_TRACE1("Attribute name: %s\n", buf);
      return len;
    }
    else
    {
      NTFS_TRACE("Unrecognized attribute name or Name buffer too small\n");
      return -1 * attr_header_.NameLength;
    }
  }
  else
  {
    NTFS_TRACE("Attribute is unnamed\n");
    return 0;
  }
}

// Get UNICODE Attribute name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int AttrBase::GetAttrName(wchar_t* buf, DWORD bufLen) const
{
  if (attr_header_.NameLength)
  {
    if (bufLen < attr_header_.NameLength)
      return -1 * attr_header_.NameLength;  // buffer too small

    bufLen = attr_header_.NameLength;
    wchar_t* namePtr =
        (wchar_t*)((BYTE*)&attr_header_ + attr_header_.NameOffset);
    wcsncpy(buf, namePtr, bufLen);
    buf[bufLen] = '\0\0';

    NTFS_TRACE("Unicode Attribute Name\n");
    return bufLen;
  }
  else
  {
    NTFS_TRACE("Attribute is unnamed\n");
    return 0;
  }
}

// Verify if this attribute is unnamed
// Useful in analyzing MultiStream files
BOOL AttrBase::IsUnNamed() const { return (attr_header_.NameLength == 0); }

WORD AttrBase::GetSectorSize() const { return sector_size_; }
DWORD AttrBase::GetClusterSize() const { return cluster_size_; }
DWORD AttrBase::GetIndexBlockSize() const { return index_block_size_; }
HANDLE AttrBase::GetHandle() const { return hvolume_; }

}  // namespace NtfsBrowser
