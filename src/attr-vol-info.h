#pragma once

#include "attr-resident.h"
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

// OK

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
  AttrVolInfo(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo(AttrVolInfo const& other) = delete;
  AttrVolInfo& operator=(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo& operator=(AttrVolInfo const& other) = delete;

  ~AttrVolInfo() override;

 private:
  BYTE major_version_;
  BYTE minor_version_;

 public:
  // Get NTFS Volume Version
  std::pair<BYTE, BYTE> GetVersion() noexcept;
};  // AttrVolInfo

}  // namespace NtfsBrowser