#pragma once

#include <vector>

#include <ntfs-browser/attr-base.h>
#include "attr-non-resident.h"

namespace NtfsBrowser
{
namespace Data
{
struct RunEntry;
}  // namespace Data
namespace Attr
{
struct HeaderNonResident;
}  // namespace Attr
////////////////////////////////
// NonResident Attributes
////////////////////////////////
class AttrNonResident : public AttrBase
{
 public:
  AttrNonResident(const AttrHeaderCommon& ahc, const FileRecord& fr);
  virtual ~AttrNonResident();

 protected:
  const Attr::HeaderNonResident& AttrHeaderNR;
  std::vector<Data::RunEntry> DataRunList;

 private:
  BOOL bDataRunOK;
  BYTE* UnalignedBuf;  // Buffer to hold not cluster aligned data
  BOOL PickData(const BYTE** dataRun, LONGLONG* length, LONGLONG* LCNOffset);
  BOOL ParseDataRun();
  BOOL ReadClusters(void* buf, DWORD clusters, LONGLONG lcn) const;
  BOOL ReadVirtualClusters(ULONGLONG vcn, DWORD clusters, void* bufv,
                           DWORD bufLen, DWORD* actural) const;

 protected:
  BOOL IsDataRunOK() const override;

 public:
  [[nodiscard]] ULONGLONG GetDataSize() const override;
  [[nodiscard]] BOOL ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                              DWORD* actural) const override;
};  // AttrNonResident
}  // namespace NtfsBrowser