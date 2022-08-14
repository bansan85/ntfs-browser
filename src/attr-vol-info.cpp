#include "attr-vol-info.h"
#include "attr/volume-information.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrVolInfo::AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr),
      vol_info_(*reinterpret_cast<const Attr::VolumeInformation*>(GetData()))
{
  NTFS_TRACE("Attribute: Volume Information\n");
}

AttrVolInfo::~AttrVolInfo() { NTFS_TRACE("AttrVolInfo deleted\n"); }

std::pair<BYTE, BYTE> AttrVolInfo::GetVersion() const noexcept
{
  return {vol_info_.major_version, vol_info_.minor_version};
}

}  // namespace NtfsBrowser
