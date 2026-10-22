#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <utility>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Data {

struct VolumeInformation;

}  // namespace Data

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrVolInfo : public Resident {
 public:
  AttrVolInfo(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrVolInfo(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo(const AttrVolInfo& other) = delete;
  AttrVolInfo& operator=(AttrVolInfo&& other) noexcept = delete;
  AttrVolInfo& operator=(const AttrVolInfo& other) = delete;

  ~AttrVolInfo() override;

  // Get NTFS Volume Version
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;

  // Get the volume flags (Data::volume_dirty_flag, ...)
  [[nodiscard]] WORD GetFlags() const noexcept;

 private:
  const Data::VolumeInformation& vol_info_;
};  // AttrVolInfo

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
