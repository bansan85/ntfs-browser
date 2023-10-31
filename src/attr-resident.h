#pragma once

#include <span>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
template <Strategy S>
class FileReader;
struct AttrHeaderCommon;
namespace Attr
{
struct HeaderResident;
}  // namespace Attr

template <Strategy S>
class AttrResident : public AttrBase<S>
{
 public:
  AttrResident(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrResident(AttrResident&& other) noexcept = delete;
  AttrResident(AttrResident const& other) = delete;
  AttrResident& operator=(AttrResident&& other) noexcept = delete;
  AttrResident& operator=(AttrResident const& other) = delete;
  ~AttrResident() override = default;

 protected:
  [[nodiscard]] bool IsDataRunOK() const noexcept override;

  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;
};  // AttrResident

class AttrResidentNoCache : public AttrResident<Strategy::NO_CACHE>
{
 public:
  AttrResidentNoCache(const AttrHeaderCommon& ahc,
                      const FileRecord<Strategy::NO_CACHE>& fr);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::span<const BYTE> body_;
};

class AttrResidentFullCache : public AttrResident<Strategy::FULL_CACHE>
{
 public:
  AttrResidentFullCache(const AttrHeaderCommon& ahc,
                        const FileRecord<Strategy::FULL_CACHE>& fr);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::vector<BYTE> body_;
};

}  // namespace NtfsBrowser
