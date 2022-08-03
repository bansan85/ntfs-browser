#include "attr-vol-info.h"
#include "ntfs-common.h"
#include "attr/volume-information.h"

namespace NtfsBrowser
{

AttrVolInfo::AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr), VolInfo(*(Attr::VolumeInformation*)attr_body_)
{
  NTFS_TRACE("Attribute: Volume Information\n");
}

AttrVolInfo::~AttrVolInfo() { NTFS_TRACE("AttrVolInfo deleted\n"); }

WORD AttrVolInfo::GetVersion()
{
  return MAKEWORD(VolInfo.MinorVersion, VolInfo.MajorVersion);
}

}  // namespace NtfsBrowser
