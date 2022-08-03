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
////////////////////////////////
// Resident Attributes
////////////////////////////////
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
  const Attr::HeaderResident& header_r_;
  std::vector<BYTE> body_;

 protected:
  [[nodiscard]] BOOL IsDataRunOK() const override;

  [[nodiscard]] ULONGLONG GetDataSize() const override;
  [[nodiscard]] BOOL ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                              DWORD* actural) const override;
  [[nodiscard]] const BYTE* GetData();
};  // AttrResident
}  // namespace NtfsBrowser