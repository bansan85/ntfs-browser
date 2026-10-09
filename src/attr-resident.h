#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
template <Cache::Strategy S>
class FileRecord;

namespace Attr {

template <Cache::Strategy S>
class AttrResident : public AttrBase<S> {
 public:
  AttrResident(const HeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrResident(AttrResident&& other) noexcept = delete;
  AttrResident(const AttrResident& other) = delete;
  AttrResident& operator=(AttrResident&& other) noexcept = delete;
  AttrResident& operator=(const AttrResident& other) = delete;
  ~AttrResident() override = default;

  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;
};  // AttrResident

class AttrResidentNoCache : public AttrResident<Cache::Strategy::NoCache> {
 public:
  AttrResidentNoCache(const HeaderCommon& ahc,
                      const FileRecord<Cache::Strategy::NoCache>& file_record);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::span<const BYTE> body_;
};

class AttrResidentFullCache : public AttrResident<Cache::Strategy::FullCache> {
 public:
  AttrResidentFullCache(
      const HeaderCommon& ahc,
      const FileRecord<Cache::Strategy::FullCache>& file_record);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::vector<BYTE> body_;
};

}  // namespace Attr

}  // namespace NtfsBrowser
