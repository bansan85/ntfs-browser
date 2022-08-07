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
  sector_size_ = fr.volume_.SectorSize;
  cluster_size_ = fr.volume_.ClusterSize;
  index_block_size_ = fr.volume_.IndexBlockSize;
  hvolume_ = fr.volume_.hVolume;
}

AttrBase::~AttrBase() {}

const AttrHeaderCommon& AttrBase::GetAttrHeader() const { return attr_header_; }

DWORD AttrBase::GetAttrType() const { return attr_header_.type; }

DWORD AttrBase::GetAttrTotalSize() const { return attr_header_.total_size; }

bool AttrBase::IsNonResident() const { return attr_header_.non_resident; }

WORD AttrBase::GetAttrFlags() const { return attr_header_.flags; }

// Get ANSI Attribute name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int AttrBase::GetAttrName(char* buf, DWORD bufLen) const
{
  if (attr_header_.name_length)
  {
    if (bufLen < attr_header_.name_length)
      return -1 * attr_header_.name_length;  // buffer too small

    wchar_t* namePtr =
        (wchar_t*)((BYTE*)&attr_header_ + attr_header_.name_offset);
    int len = WideCharToMultiByte(CP_ACP, 0, namePtr, attr_header_.name_length,
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
      return -1 * attr_header_.name_length;
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
  if (attr_header_.name_length)
  {
    if (bufLen < attr_header_.name_length)
      return -1 * attr_header_.name_length;  // buffer too small

    bufLen = attr_header_.name_length;
    wchar_t* namePtr =
        (wchar_t*)((BYTE*)&attr_header_ + attr_header_.name_offset);
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
bool AttrBase::IsUnNamed() const { return (attr_header_.name_length == 0); }

WORD AttrBase::GetSectorSize() const { return sector_size_; }
DWORD AttrBase::GetClusterSize() const { return cluster_size_; }
DWORD AttrBase::GetIndexBlockSize() const { return index_block_size_; }
HANDLE AttrBase::GetHandle() const { return hvolume_; }

}  // namespace NtfsBrowser
