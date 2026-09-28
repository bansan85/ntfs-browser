#include "attr-std-info.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <stdexcept>

#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "attr/standard-information.h"
#include "flag/std-info-permission.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <typename RESIDENT, Strategy S>
AttrStdInfo<RESIDENT, S>::AttrStdInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
      std_info_(
          *reinterpret_cast<const Attr::StandardInformation*>(this->GetData()))
{
  if (this->GetDataSize() < offsetof(Attr::StandardInformation, owner_id))
  {
    throw std::runtime_error(
        "Standard Information attribute smaller than expected.\n");
  }

  LogTrace("Attribute: Standard Information");
}

template <typename RESIDENT, Strategy S>
AttrStdInfo<RESIDENT, S>::~AttrStdInfo()
{
  LogTrace("AttrStdInfo deleted");
}

// Change from UTC time to local time
template <typename RESIDENT, Strategy S>
void AttrStdInfo<RESIDENT, S>::GetFileTime(FILETIME* writeTm,
                                           FILETIME* createTm,
                                           FILETIME* accessTm,
                                           FILETIME* changeTm) const noexcept
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

  if (changeTm != nullptr)
  {
    UTC2Local(std_info_.mft_time, *changeTm);
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
bool AttrStdInfo<RESIDENT, S>::IsArchive() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::ARCHIVE);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsDevice() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::DEVICE);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsNormal() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::NORMAL);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsTemporary() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::TEMP);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsCompressed() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::COMPRESSED);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsOffline() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::OFFLINE);
}

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsNotContentIndexed() const noexcept
{
  return static_cast<bool>(std_info_.permission & Flag::StdInfoPermission::NCI);
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

template <typename RESIDENT, Strategy S>
bool AttrStdInfo<RESIDENT, S>::IsReparsePoint() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::REPARSE);
}

// UTC filetime to Local filetime
template <typename RESIDENT, Strategy S>
void AttrStdInfo<RESIDENT, S>::UTC2Local(const ULONGLONG& ultm,
                                         FILETIME& lftm) noexcept
{
#ifdef _WIN32
  const _ULARGE_INTEGER fti{.QuadPart = ultm};
  FILETIME ftt{.dwLowDateTime = fti.LowPart, .dwHighDateTime = fti.HighPart};

  if (FileTimeToLocalFileTime(&ftt, &lftm) == 0)
  {
    lftm = ftt;
  }
#else
  // No portable timezone conversion outside Windows; time stays in UTC.
  lftm.dwLowDateTime = static_cast<DWORD>(ultm & 0xFFFFFFFFULL);
  lftm.dwHighDateTime = static_cast<DWORD>(ultm >> 32);
#endif
}

template class AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
