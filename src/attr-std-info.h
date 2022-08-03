#pragma once

#include <windows.h>

#include "attr-resident.h"
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser
{
namespace Attr
{
struct StandardInformation;
}  // namespace Attr
namespace Flag
{
enum class StdInfoPermission : DWORD;
}  // namespace Flag

///////////////////////////////////
// Attribute: Standard Information
///////////////////////////////////
class AttrStdInfo : public AttrResident
{
 public:
  AttrStdInfo(const AttrHeaderCommon& ahc, const FileRecord& fr);
  virtual ~AttrStdInfo();

 private:
  const Attr::StandardInformation& StdInfo;

 public:
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;
  Flag::StdInfoPermission GetFilePermission() const;
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;

  static void UTC2Local(const ULONGLONG& ultm, FILETIME* lftm);
};  // AttrStdInfo
}  // namespace NtfsBrowser