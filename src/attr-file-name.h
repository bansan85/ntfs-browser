#pragma once

#include "attr-resident.h"
#include <ntfs-browser/filename.h>
#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon;

////////////////////////////////
// Attribute: File Name
////////////////////////////////
class AttrFileName : public AttrResident, public Filename
{
 public:
  AttrFileName(const AttrHeaderCommon& ahc, const FileRecord& fr);

  virtual ~AttrFileName();

 private:
  // File permission and time in $FILE_NAME only updates when the filename changes
  // So hide these functions to prevent user from getting the error information
  // Standard Information and IndexEntry keeps the most recent file time and permission infomation
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;
  DWORD GetFilePermission();
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;
};  // AttrFileName

}  // namespace NtfsBrowser