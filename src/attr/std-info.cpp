#include "attr/std-info.h"

#include <cstddef>
#include <stdexcept>

#include <ntfs-browser/namespace.h>

#include "attr/resident.h"
#include "cache/strategy.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "log/ntfs-common.h"
#include "util/local-file-time.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Attr {

namespace {

// Checks the body size before a reference is bound to it: an empty body may
// have a null data pointer, which a reference MUST NOT be bound to.
template <typename Resident>
const Data::StandardInformation& CheckedStdInfo(const Resident& attr) {
  if (attr.GetDataSize() < offsetof(Data::StandardInformation, owner_id)) {
    throw std::runtime_error(
        "Standard Information attribute smaller than expected.\n");
  }

  return *reinterpret_cast<const Data::StandardInformation*>(attr.GetData());
}

}  // namespace

template <typename Resident, Cache::Strategy S>
AttrStdInfo<Resident, S>::AttrStdInfo(const HeaderCommon& ahc,
                                      const Io::FileRecord<S>& file_record)
    : Resident(ahc, file_record), std_info_(CheckedStdInfo<Resident>(*this)) {
  Log::Trace("Attribute: Standard Information");
}

template <typename Resident, Cache::Strategy S>
AttrStdInfo<Resident, S>::~AttrStdInfo() {
  Log::Trace("AttrStdInfo deleted");
}

// Change from UTC time to local time
template <typename Resident, Cache::Strategy S>
void AttrStdInfo<Resident, S>::GetFileTime(FILETIME* write_tm,
                                           FILETIME* create_tm,
                                           FILETIME* access_tm,
                                           FILETIME* change_tm) const noexcept {
  if (write_tm != nullptr) {
    UTC2Local(std_info_.alter_time, *write_tm);
  }

  if (create_tm != nullptr) {
    UTC2Local(std_info_.create_time, *create_tm);
  }

  if (access_tm != nullptr) {
    UTC2Local(std_info_.read_time, *access_tm);
  }

  if (change_tm != nullptr) {
    UTC2Local(std_info_.mft_time, *change_tm);
  }
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsReadOnly() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::ReadOnly);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsHidden() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Hidden);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsSystem() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::System);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsArchive() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Archive);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsDevice() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Device);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsNormal() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Normal);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsTemporary() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Temp);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsCompressed() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Compressed);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsOffline() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Offline);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsNotContentIndexed() const noexcept {
  return static_cast<bool>(std_info_.permission & Data::StdInfoPermission::Nci);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsEncrypted() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Encrypted);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsSparse() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Sparse);
}

template <typename Resident, Cache::Strategy S>
bool AttrStdInfo<Resident, S>::IsReparsePoint() const noexcept {
  return static_cast<bool>(std_info_.permission &
                           Data::StdInfoPermission::Reparse);
}

// UTC filetime to Local filetime
template <typename Resident, Cache::Strategy S>
void AttrStdInfo<Resident, S>::UTC2Local(const ULONGLONG& ultm,
                                         FILETIME& lftm) noexcept {
  Util::UtcToLocalFileTime(ultm, lftm);
}

template class AttrStdInfo<AttrResidentFullCache, Cache::Strategy::FullCache>;
template class AttrStdInfo<AttrResidentNoCache, Cache::Strategy::NoCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
