#pragma once

#include "attr-resident.h"
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{
namespace Attr
{
struct VolumeInformation;
}  // namespace Attr

//////////////////////////////////
// Attribute: Volume Information
//////////////////////////////////
class AttrVolInfo : public AttrResident
{
 public:
  AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord& fr);

  virtual ~AttrVolInfo();

 private:
  const Attr::VolumeInformation& VolInfo;

 public:
  // Get NTFS Volume Version
  WORD GetVersion();
};  // AttrVolInfo

}  // namespace NtfsBrowser