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
  virtual ~AttrResident() = default;

 protected:
  const Attr::HeaderResident& AttrHeaderR;
  const void* AttrBody;  // Points to Resident Data
  DWORD AttrBodySize;    // Attribute Data Size

  virtual BOOL IsDataRunOK() const;

 public:
  virtual ULONGLONG GetDataSize() const;
  virtual BOOL ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                        DWORD* actural) const;
};  // AttrResident
}  // namespace NtfsBrowser