#include "attr-std-info.h"
#include "ntfs-common.h"

#include "attr/standard-information.h"
#include "flag/std-info-permission.h"

namespace NtfsBrowser
{

AttrStdInfo::AttrStdInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr), StdInfo(*(Attr::StandardInformation*)GetData())
{
  NTFS_TRACE("Attribute: Standard Information\n");
}

AttrStdInfo::~AttrStdInfo() { NTFS_TRACE("AttrStdInfo deleted\n"); }

// Change from UTC time to local time
void AttrStdInfo::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                              FILETIME* accessTm) const
{
  if (writeTm) UTC2Local(StdInfo.AlterTime, writeTm);

  if (createTm) UTC2Local(StdInfo.CreateTime, createTm);

  if (accessTm) UTC2Local(StdInfo.ReadTime, accessTm);
}

Flag::StdInfoPermission AttrStdInfo::GetFilePermission() const
{
  return StdInfo.Permission;
}

BOOL AttrStdInfo::IsReadOnly() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::READONLY);
}

BOOL AttrStdInfo::IsHidden() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::HIDDEN);
}

BOOL AttrStdInfo::IsSystem() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::SYSTEM);
}

BOOL AttrStdInfo::IsCompressed() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::COMPRESSED);
}

BOOL AttrStdInfo::IsEncrypted() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::ENCRYPTED);
}

BOOL AttrStdInfo::IsSparse() const
{
  return static_cast<BOOL>(StdInfo.Permission &
                           Flag::StdInfoPermission::SPARSE);
}

// UTC filetime to Local filetime
void AttrStdInfo::UTC2Local(const ULONGLONG& ultm, FILETIME* lftm)
{
  LARGE_INTEGER fti;
  FILETIME ftt;

  fti.QuadPart = ultm;
  ftt.dwHighDateTime = fti.HighPart;
  ftt.dwLowDateTime = fti.LowPart;

  if (!FileTimeToLocalFileTime(&ftt, lftm)) *lftm = ftt;
}

}  // namespace NtfsBrowser