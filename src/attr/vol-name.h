#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/cache/strategy.h>  // IWYU pragma: keep
#include <ntfs-browser/io/file-record.h>

namespace NtfsBrowser {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
template <Cache::Strategy S>
class NtfsVolume;

namespace Attr {

template <typename Resident, Cache::Strategy S>
class AttrVolName : public Resident {
 public:
  AttrVolName(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrVolName(AttrVolName&& other) noexcept = delete;
  AttrVolName(const AttrVolName& other) = delete;
  AttrVolName& operator=(AttrVolName&& other) noexcept = delete;
  AttrVolName& operator=(const AttrVolName& other) = delete;
  ~AttrVolName() override = default;

  template <Cache::Strategy>
  friend class NtfsBrowser::NtfsVolume;

 private:
  std::wstring name_;

  // Get NTFS Volume Unicode Name
  [[nodiscard]] std::wstring_view GetName() const noexcept;
};  // AttrVolInfo

}  // namespace Attr

}  // namespace NtfsBrowser
