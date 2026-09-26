#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

#include "attr-resident.h"

namespace NtfsBrowser
{
namespace Attr
{
struct VolumeInformation;
}  // namespace Attr

template <typename RESIDENT, Strategy S>
class AttrVolInfo : public RESIDENT
{
 public:
  AttrVolInfo(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrVolInfo(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo(AttrVolInfo const& other) = delete;
  AttrVolInfo& operator=(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo& operator=(AttrVolInfo const& other) = delete;

  ~AttrVolInfo() override;

  friend class NtfsVolume<S>;

 private:
  const Attr::VolumeInformation& vol_info_;

  // Get NTFS Volume Version
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
};  // AttrVolInfo

}  // namespace NtfsBrowser