#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <Strategy S>
class AttrResident : public AttrBase<S>
{
 public:
  AttrResident(const AttrHeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrResident(AttrResident&& other) noexcept = delete;
  AttrResident(AttrResident const& other) = delete;
  AttrResident& operator=(AttrResident&& other) noexcept = delete;
  AttrResident& operator=(AttrResident const& other) = delete;
  ~AttrResident() override = default;

  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;
};  // AttrResident

class AttrResidentNoCache : public AttrResident<Strategy::NoCache>
{
 public:
  AttrResidentNoCache(const AttrHeaderCommon& ahc,
                      const FileRecord<Strategy::NoCache>& file_record);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::span<const BYTE> body_;
};

class AttrResidentFullCache : public AttrResident<Strategy::FullCache>
{
 public:
  AttrResidentFullCache(const AttrHeaderCommon& ahc,
                        const FileRecord<Strategy::FullCache>& file_record);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::vector<BYTE> body_;
};

}  // namespace NtfsBrowser
