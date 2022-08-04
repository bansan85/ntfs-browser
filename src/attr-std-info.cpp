#include "attr-std-info.h"
#include "ntfs-common.h"

#include "attr/standard-information.h"
#include "flag/std-info-permission.h"

// OK

namespace NtfsBrowser
{

AttrStdInfo::AttrStdInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr),
      std_info_(*reinterpret_cast<const Attr::StandardInformation*>(GetData()))
{
  NTFS_TRACE("Attribute: Standard Information\n");
}

AttrStdInfo::~AttrStdInfo() { NTFS_TRACE("AttrStdInfo deleted\n"); }

// Change from UTC time to local time
void AttrStdInfo::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                              FILETIME* accessTm) const noexcept
{
  if (writeTm != nullptr)
  {
    UTC2Local(std_info_.AlterTime, *writeTm);
  }

  if (createTm != nullptr)
  {
    UTC2Local(std_info_.CreateTime, *createTm);
  }

  if (accessTm != nullptr)
  {
    UTC2Local(std_info_.ReadTime, *accessTm);
  }
}

Flag::StdInfoPermission AttrStdInfo::GetFilePermission() const noexcept
{
  return std_info_.Permission;
}

BOOL AttrStdInfo::IsReadOnly() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::READONLY);
}

BOOL AttrStdInfo::IsHidden() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::HIDDEN);
}

BOOL AttrStdInfo::IsSystem() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::SYSTEM);
}

BOOL AttrStdInfo::IsCompressed() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::COMPRESSED);
}

BOOL AttrStdInfo::IsEncrypted() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::ENCRYPTED);
}

BOOL AttrStdInfo::IsSparse() const noexcept
{
  return static_cast<BOOL>(std_info_.Permission &
                           Flag::StdInfoPermission::SPARSE);
}

// UTC filetime to Local filetime
void AttrStdInfo::UTC2Local(const ULONGLONG& ultm, FILETIME& lftm) noexcept
{
  const _ULARGE_INTEGER fti{.QuadPart = ultm};
  FILETIME ftt{.dwLowDateTime = fti.LowPart, .dwHighDateTime = fti.HighPart};

  if (FileTimeToLocalFileTime(&ftt, &lftm) == 0)
  {
    lftm = ftt;
  }
}

}  // namespace NtfsBrowser
