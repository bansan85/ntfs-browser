#pragma once

#include <ntfs-browser/attr-resident.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-volume-information.h>

namespace NtfsBrowser
{

//////////////////////////////////
// Attribute: Volume Information
//////////////////////////////////
class AttrVolInfo : public AttrResident
{
 public:
  AttrVolInfo(const AttrHeaderCommon* ahc, const FileRecord* fr)
      : AttrResident(ahc, fr)
  {
    NTFS_TRACE("Attribute: Volume Information\n");

    VolInfo = (AttrVolumeInformation*)AttrBody;
  }

  virtual ~AttrVolInfo() { NTFS_TRACE("AttrVolInfo deleted\n"); }

 private:
  const AttrVolumeInformation* VolInfo;

 public:
  // Get NTFS Volume Version
  WORD GetVersion()
  {
    return MAKEWORD(VolInfo->MinorVersion, VolInfo->MajorVersion);
  }
};  // AttrVolInfo

}  // namespace NtfsBrowser