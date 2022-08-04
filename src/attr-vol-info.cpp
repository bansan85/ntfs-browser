#include "attr-vol-info.h"
#include "attr/volume-information.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

AttrVolInfo::AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr),
      vol_info_(*reinterpret_cast<const Attr::VolumeInformation*>(GetData()))
{
  NTFS_TRACE("Attribute: Volume Information\n");
}

AttrVolInfo::~AttrVolInfo() { NTFS_TRACE("AttrVolInfo deleted\n"); }

std::pair<BYTE, BYTE> AttrVolInfo::GetVersion() noexcept
{
  return {vol_info_.MajorVersion, vol_info_.MinorVersion};
}

}  // namespace NtfsBrowser
