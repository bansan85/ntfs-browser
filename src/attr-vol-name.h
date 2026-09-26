#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/strategy.h>

#include "attr-resident.h"

namespace NtfsBrowser
{
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;
template <Strategy S>
class NtfsVolume;

template <typename RESIDENT, Strategy S>
class AttrVolName : public RESIDENT
{
 public:
  AttrVolName(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrVolName(AttrVolName&& other) noexcept = delete;
  AttrVolName(AttrVolName const& other) = delete;
  AttrVolName& operator=(AttrVolName&& other) noexcept = delete;
  AttrVolName& operator=(AttrVolName const& other) = delete;
  ~AttrVolName() override = default;

  // NtfsVolume::Init() picks between both Strategy instantiations of this
  // class via a runtime if/else on S, not `if constexpr`, so every
  // NtfsVolume<S> must be a friend regardless of this instantiation's own S.
  template <Strategy>
  friend class NtfsVolume;

 private:
  std::wstring name_;

  // Get NTFS Volume Unicode Name
  [[nodiscard]] std::wstring_view GetName() const noexcept;
};  // AttrVolInfo

}  // namespace NtfsBrowser