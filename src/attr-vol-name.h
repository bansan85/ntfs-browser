#pragma once

#include "attr-resident.h"
#include <string>
#include <string_view>

namespace NtfsBrowser
{
struct AttrHeaderCommon;
class FileRecord;

template <typename RESIDENT>
class AttrVolName : public RESIDENT
{
 public:
  AttrVolName(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrVolName(AttrVolName&& other) noexcept = delete;
  AttrVolName(AttrVolName const& other) = delete;
  AttrVolName& operator=(AttrVolName&& other) noexcept = delete;
  AttrVolName& operator=(AttrVolName const& other) = delete;
  ~AttrVolName() override = default;

 private:
  std::wstring name_;

 public:
  // Get NTFS Volume Unicode Name
  [[nodiscard]] std::wstring_view GetName() const noexcept;
};  // AttrVolInfo

}  // namespace NtfsBrowser