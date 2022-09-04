#pragma once

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

 private:
  std::vector<BYTE> body_;

 protected:
  [[nodiscard]] bool IsDataRunOK() const noexcept override;

  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, std::span<BYTE> buffer) const override;
  [[nodiscard]] const BYTE* GetData() const noexcept;
};  // AttrResident
}  // namespace NtfsBrowser