#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "attr/forward.h"
#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

template <typename Resident, Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY AttrVolName : public Resident {
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
