#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/ntfs-common.h>

namespace NtfsBrowser
{

AttrBase::AttrBase(const AttrHeaderCommon* ahc, const FileRecord* fr)
{
  _ASSERT(ahc);
  _ASSERT(fr);

  AttrHeader = ahc;
  file_record_ = fr;

  _SectorSize = fr->Volume->SectorSize;
  _ClusterSize = fr->Volume->ClusterSize;
  _IndexBlockSize = fr->Volume->IndexBlockSize;
  _hVolume = fr->Volume->hVolume;
}

AttrBase::~AttrBase() {}

const AttrHeaderCommon* AttrBase::GetAttrHeader() const { return AttrHeader; }

DWORD AttrBase::GetAttrType() const { return AttrHeader->Type; }

DWORD AttrBase::GetAttrTotalSize() const { return AttrHeader->TotalSize; }

BOOL AttrBase::IsNonResident() const { return AttrHeader->NonResident; }

WORD AttrBase::GetAttrFlags() const { return AttrHeader->Flags; }

// Get ANSI Attribute name
// Return 0: Unnamed, <0: buffer too small, -buffersize, >0 Name length
int AttrBase::GetAttrName(char* buf, DWORD bufLen) const
{
  if (AttrHeader->NameLength)
  {
    if (bufLen < AttrHeader->NameLength)
      return -1 * AttrHeader->NameLength;  // buffer too small

    wchar_t* namePtr = (wchar_t*)((BYTE*)AttrHeader + AttrHeader->NameOffset);
    int len = WideCharToMultiByte(CP_ACP, 0, namePtr, AttrHeader->NameLength,
                                  buf, bufLen, NULL, NULL);
    if (len)
    {
      buf[len] = '\0';
      NTFS_TRACE1("Attribute name: %s\n", buf);
      return len;
    }
    else
    {
      NTFS_TRACE("Unrecognized attribute name or Name buffer too small\n");
      return -1 * AttrHeader->NameLength;
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
  if (AttrHeader->NameLength)
  {
    if (bufLen < AttrHeader->NameLength)
      return -1 * AttrHeader->NameLength;  // buffer too small

    bufLen = AttrHeader->NameLength;
    wchar_t* namePtr = (wchar_t*)((BYTE*)AttrHeader + AttrHeader->NameOffset);
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
BOOL AttrBase::IsUnNamed() const { return (AttrHeader->NameLength == 0); }
}  // namespace NtfsBrowser
