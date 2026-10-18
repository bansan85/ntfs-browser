#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/cache/strategy.h>  // IWYU pragma: keep
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

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

  // Get NTFS Volume Unicode Name
  [[nodiscard]] std::wstring_view GetName() const noexcept;

 private:
  std::wstring name_;
};  // AttrVolInfo

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
