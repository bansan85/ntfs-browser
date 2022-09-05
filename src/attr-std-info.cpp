#include "attr-std-info.h"
#include "ntfs-common.h"

#include "attr/standard-information.h"
#include "flag/std-info-permission.h"

namespace NtfsBrowser
{

template <typename RESIDENT>
AttrStdInfo<RESIDENT>::AttrStdInfo(const AttrHeaderCommon& ahc,
                                   const FileRecord& fr)
    : RESIDENT(ahc, fr),
      std_info_(
          *reinterpret_cast<const Attr::StandardInformation*>(this->GetData()))
{
  NTFS_TRACE("Attribute: Standard Information\n");
}

template <typename RESIDENT>
AttrStdInfo<RESIDENT>::~AttrStdInfo()
{
  NTFS_TRACE("AttrStdInfo deleted\n");
}

// Change from UTC time to local time
template <typename RESIDENT>
void AttrStdInfo<RESIDENT>::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
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

template <typename RESIDENT>
Flag::StdInfoPermission
    AttrStdInfo<RESIDENT>::GetFilePermission() const noexcept
{
  return std_info_.permission;
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsReadOnly() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::READONLY);
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsHidden() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::HIDDEN);
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsSystem() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::SYSTEM);
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsCompressed() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::COMPRESSED);
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsEncrypted() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::ENCRYPTED);
}

template <typename RESIDENT>
bool AttrStdInfo<RESIDENT>::IsSparse() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::SPARSE);
}

// UTC filetime to Local filetime
template <typename RESIDENT>
void AttrStdInfo<RESIDENT>::UTC2Local(const ULONGLONG& ultm,
                                      FILETIME& lftm) noexcept
{
  const _ULARGE_INTEGER fti{.QuadPart = ultm};
  FILETIME ftt{.dwLowDateTime = fti.LowPart, .dwHighDateTime = fti.HighPart};

  if (FileTimeToLocalFileTime(&ftt, &lftm) == 0)
  {
    lftm = ftt;
  }
}

template class AttrStdInfo<AttrResidentHeavy>;
template class AttrStdInfo<AttrResidentLight>;

}  // namespace NtfsBrowser
