#pragma once

#include <span>
#include <vector>

#include <ntfs-browser/attr-base.h>

namespace NtfsBrowser
{
class FileReader;
struct AttrHeaderCommon;
namespace Attr
{
struct HeaderResident;
}  // namespace Attr

class AttrResident : public AttrBase
{
 public:
  AttrResident(const AttrHeaderCommon& ahc, const FileRecord& fr);
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

class AttrResidentLight : public AttrResident
{
 public:
  AttrResidentLight(const AttrHeaderCommon& ahc, const FileRecord& fr);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::span<const BYTE> body_;
};

class AttrResidentHeavy : public AttrResident
{
 public:
  AttrResidentHeavy(const AttrHeaderCommon& ahc, const FileRecord& fr);
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;

 private:
  std::vector<BYTE> body_;
};

}  // namespace NtfsBrowser
