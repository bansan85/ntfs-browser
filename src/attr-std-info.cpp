#include <ntfs-browser/attr-std-info.h>
#include <ntfs-browser/data/attr-std-info-permission.h>
#include <ntfs-browser/ntfs-common.h>

namespace NtfsBrowser
{

AttrStdInfo::AttrStdInfo(const AttrHeaderCommon* ahc, const FileRecord* fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Standard Information\n");

  StdInfo = (AttrStandardInformation*)AttrBody;
}

AttrStdInfo::~AttrStdInfo() { NTFS_TRACE("AttrStdInfo deleted\n"); }

// Change from UTC time to local time
void AttrStdInfo::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                              FILETIME* accessTm) const
{
  UTC2Local(StdInfo->AlterTime, writeTm);

  if (createTm) UTC2Local(StdInfo->CreateTime, createTm);

  if (accessTm) UTC2Local(StdInfo->ReadTime, accessTm);
}

DWORD AttrStdInfo::GetFilePermission() const { return StdInfo->Permission; }

BOOL AttrStdInfo::IsReadOnly() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::READONLY));
}

BOOL AttrStdInfo::IsHidden() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::HIDDEN));
}

BOOL AttrStdInfo::IsSystem() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::SYSTEM));
}

BOOL AttrStdInfo::IsCompressed() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::COMPRESSED));
}

BOOL AttrStdInfo::IsEncrypted() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::ENCRYPTED));
}

BOOL AttrStdInfo::IsSparse() const
{
  return ((StdInfo->Permission) &
          static_cast<DWORD>(AttrStdInfoPermission::SPARSE));
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