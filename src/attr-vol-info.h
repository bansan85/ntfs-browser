#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <utility>

namespace NtfsBrowser {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
template <Cache::Strategy S>
class FileRecord;

namespace Data {

struct VolumeInformation;

}  // namespace Data

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrVolInfo : public Resident {
 public:
  AttrVolInfo(const HeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrVolInfo(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo(const AttrVolInfo& other) = delete;
  AttrVolInfo& operator=(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo& operator=(const AttrVolInfo& other) = delete;

  ~AttrVolInfo() override;

  template <Cache::Strategy>
  friend class NtfsVolume;

 private:
  const Data::VolumeInformation& vol_info_;

  // Get NTFS Volume Version
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
};  // AttrVolInfo

}  // namespace Attr

}  // namespace NtfsBrowser
