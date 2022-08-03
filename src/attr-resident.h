#pragma once

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
  ~AttrResident() override = default;

 protected:
  const Attr::HeaderResident& attr_header_r_;
  const void* attr_body_;  // Points to Resident Data
  DWORD attr_body_size_;   // Attribute Data Size

  [[nodiscard]] BOOL IsDataRunOK() const override;

 public:
  [[nodiscard]] ULONGLONG GetDataSize() const override;
  [[nodiscard]] BOOL ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                              DWORD* actural) const override;
};  // AttrResident
}  // namespace NtfsBrowser