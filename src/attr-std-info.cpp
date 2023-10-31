#include "attr-std-info.h"
#include "ntfs-common.h"

#include "attr/standard-information.h"
#include "flag/std-info-permission.h"

namespace NtfsBrowser
{

template <typename RESIDENT, Strategy S>
AttrStdInfo<RESIDENT, S>::AttrStdInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
      std_info_(
          *reinterpret_cast<const Attr::StandardInformation*>(this->GetData()))
{
  NTFS_TRACE("Attribute: Standard Information\n");
}

template <typename RESIDENT, Strategy S>
AttrStdInfo<RESIDENT, S>::~AttrStdInfo()
{
  NTFS_TRACE("AttrStdInfo deleted\n");
}

// Change from UTC time to local time
template <typename RESIDENT, Strategy S>
void AttrStdInfo<RESIDENT, S>::GetFileTime(FILETIME* writeTm,
                                           FILETIME* createTm,
                                           FILETIME* accessTm) const noexcept
{
  if (writeTm != nullptr)
  {
    UTC2Local(std_info_.alter_time, *writeTm);
  }

  if (createTm != nullptr)
  {
    UTC2Local(std_info_.create_time, *createTm);
  }

  if (accessTm != nullptr)
  {
    UTC2Local(std_info_.read_time, *accessTm);
  }
}

template <typename RESIDENT, Strategy S>
Flag::StdInfoPermission
    AttrStdInfo<RESIDENT, S>::GetFilePermission() const noexcept
{
  return std_info_.permission;
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsReadOnly() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::READONLY);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsHidden() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::HIDDEN);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsSystem() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::SYSTEM);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsCompressed() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::COMPRESSED);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsEncrypted() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::ENCRYPTED);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsSparse() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::SPARSE);
}

// UTC filetime to Local filetime
template <typename RESIDENT, Strategy S>
void AttrStdInfo<RESIDENT, S>::UTC2Local(const ULONGLONG& ultm,
                                         FILETIME& lftm) noexcept
{
  const _ULARGE_INTEGER fti{.QuadPart = ultm};
  FILETIME ftt{.dwLowDateTime = fti.LowPart, .dwHighDateTime = fti.HighPart};

  if (FileTimeToLocalFileTime(&ftt, &lftm) == 0)
  {
    lftm = ftt;
  }
}

template class AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
