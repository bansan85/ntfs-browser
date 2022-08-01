#pragma once

#include <windows.h>

#include <ntfs-browser/attr-resident.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-standard-information.h>

namespace NtfsBrowser
{
///////////////////////////////////
// Attribute: Standard Information
///////////////////////////////////
class AttrStdInfo : public AttrResident
{
 public:
  AttrStdInfo(const AttrHeaderCommon* ahc, const FileRecord* fr);
  virtual ~AttrStdInfo();

 private:
  const AttrStandardInformation* StdInfo;

 public:
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;
  DWORD GetFilePermission() const;
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;

  static void UTC2Local(const ULONGLONG& ultm, FILETIME* lftm);
};  // AttrStdInfo
}  // namespace NtfsBrowser