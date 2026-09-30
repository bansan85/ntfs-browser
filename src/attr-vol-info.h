#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <utility>

namespace NtfsBrowser
{
enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

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

  template <Strategy>
  friend class NtfsVolume;

 private:
  const Attr::VolumeInformation& vol_info_;

  // Get NTFS Volume Version
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
};  // AttrVolInfo

}  // namespace NtfsBrowser