#pragma once

#include "attr-resident.h"
#include <ntfs-browser/filename.h>
#include <ntfs-browser/data/attr-header-common.h>

// OK

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
  AttrFileName(AttrFileName&& other) noexcept = delete;
  AttrFileName(AttrFileName const& other) = delete;
  AttrFileName& operator=(AttrFileName&& other) noexcept = delete;
  AttrFileName& operator=(AttrFileName const& other) = delete;
  virtual ~AttrFileName();

 private:
  // File permission and time in $FILE_NAME only updates when the filename changes
  // So hide these functions to prevent user from getting the error information
  // Standard Information and IndexEntry keeps the most recent file time and permission infomation
#if 0
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept override;
  Flag::Filename GetFilePermission() const noexcept override;
  bool IsReadOnly() const noexcept override;
  bool IsHidden() const noexcept override;
  bool IsSystem() const noexcept override;
  bool IsCompressed() const noexcept override;
  bool IsEncrypted() const noexcept override;
  bool IsSparse() const noexcept override;
#endif
};  // AttrFileName

}  // namespace NtfsBrowser