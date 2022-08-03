#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
class FileRecord;

class AttrBase
{
 public:
  AttrBase(const AttrHeaderCommon& ahc, const FileRecord& fr);
  virtual ~AttrBase();

 protected:
  const AttrHeaderCommon& AttrHeader;
  WORD _SectorSize;
  DWORD _ClusterSize;
  DWORD _IndexBlockSize;
  HANDLE _hVolume;
  const FileRecord& file_record_;

 public:
  const AttrHeaderCommon& GetAttrHeader() const;
  DWORD GetAttrType() const;
  DWORD GetAttrTotalSize() const;
  BOOL IsNonResident() const;
  WORD GetAttrFlags() const;
  int GetAttrName(char* buf, DWORD bufLen) const;
  int GetAttrName(wchar_t* buf, DWORD bufLen) const;
  BOOL IsUnNamed() const;

 protected:
  virtual BOOL IsDataRunOK() const = 0;

 public:
  virtual ULONGLONG GetDataSize() const = 0;
  virtual BOOL ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                        DWORD* actural) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
