#include "attr-vol-info.h"
#include "attr/volume-information.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

AttrVolInfo::AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Volume Information\n");
  const auto* vol_info =
      reinterpret_cast<const Attr::VolumeInformation*>(GetData());
  major_version_ = vol_info->MajorVersion;
  minor_version_ = vol_info->MinorVersion;
}

AttrVolInfo::~AttrVolInfo() { NTFS_TRACE("AttrVolInfo deleted\n"); }

std::pair<BYTE, BYTE> AttrVolInfo::GetVersion() noexcept
{
  return {minor_version_, major_version_};
}

}  // namespace NtfsBrowser
