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

namespace
{
// Selects the low DWORD of a 64-bit FILETIME value.
constexpr ULONGLONG low_dword_mask = 0xFFFFFFFFULL;

// Bits in a DWORD: where the high half of a FILETIME value starts.
constexpr unsigned dword_bits = 32;

// Checks the body size before a reference is bound to it: an empty body may
// have a null data pointer, which a reference MUST NOT be bound to.
template <typename Resident>
const Attr::StandardInformation& CheckedStdInfo(const Resident& attr)
{
  if (attr.GetDataSize() < offsetof(Attr::StandardInformation, owner_id))
  {
    throw std::runtime_error(
        "Standard Information attribute smaller than expected.\n");
  }

  return *reinterpret_cast<const Attr::StandardInformation*>(attr.GetData());
}
}  // namespace

template <typename Resident, Strategy S>
AttrStdInfo<Resident, S>::AttrStdInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& file_record)
    : Resident(ahc, file_record), std_info_(CheckedStdInfo<Resident>(*this))
{
  LogTrace("Attribute: Standard Information");
}

template <typename Resident, Strategy S>
AttrStdInfo<Resident, S>::~AttrStdInfo()
{
  LogTrace("AttrStdInfo deleted");
}

// Change from UTC time to local time
template <typename Resident, Strategy S>
void AttrStdInfo<Resident, S>::GetFileTime(FILETIME* write_tm,
                                           FILETIME* create_tm,
                                           FILETIME* access_tm,
                                           FILETIME* change_tm) const noexcept
{
  if (write_tm != nullptr)
  {
    UTC2Local(std_info_.alter_time, *write_tm);
  }

  if (create_tm != nullptr)
  {
    UTC2Local(std_info_.create_time, *create_tm);
  }

  if (access_tm != nullptr)
  {
    UTC2Local(std_info_.read_time, *access_tm);
  }

  if (change_tm != nullptr)
  {
    UTC2Local(std_info_.mft_time, *change_tm);
  }
}

template <typename Resident, Strategy S>
Flag::StdInfoPermission
    AttrStdInfo<Resident, S>::GetFilePermission() const noexcept
{
  return std_info_.permission;
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsReadOnly() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::ReadOnly);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsHidden() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Hidden);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsSystem() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::System);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsArchive() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Archive);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsDevice() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Device);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsNormal() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Normal);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsTemporary() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Temp);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsCompressed() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Compressed);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsOffline() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Offline);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsNotContentIndexed() const noexcept
{
  return static_cast<bool>(std_info_.permission & Flag::StdInfoPermission::Nci);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsEncrypted() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Encrypted);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsSparse() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Sparse);
}

template <typename Resident, Strategy S>
bool AttrStdInfo<Resident, S>::IsReparsePoint() const noexcept
{
  return static_cast<bool>(std_info_.permission &
                           Flag::StdInfoPermission::Reparse);
}

// UTC filetime to Local filetime
template <typename Resident, Strategy S>
void AttrStdInfo<Resident, S>::UTC2Local(const ULONGLONG& ultm,
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
  lftm.dwLowDateTime = static_cast<DWORD>(ultm & low_dword_mask);
  lftm.dwHighDateTime = static_cast<DWORD>(ultm >> dword_bits);
#endif
}

template class AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>;
template class AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>;

}  // namespace NtfsBrowser
