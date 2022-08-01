#pragma once

#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-non-resident.h>
#include <ntfs-browser/data/data-run-entry.h>
#include <ntfs-browser/attr-non-resident.h>

namespace NtfsBrowser
{
////////////////////////////////
// NonResident Attributes
////////////////////////////////
class AttrNonResident : public AttrBase
{
 public:
  AttrNonResident(const AttrHeaderCommon* ahc, const FileRecord* fr);
  virtual ~AttrNonResident();

 protected:
  const AttrHeaderNonResident* AttrHeaderNR;
  std::vector<DataRunEntry> DataRunList;

 private:
  BOOL bDataRunOK;
  BYTE* UnalignedBuf;  // Buffer to hold not cluster aligned data
  BOOL PickData(const BYTE** dataRun, LONGLONG* length, LONGLONG* LCNOffset);
  BOOL ParseDataRun();
  BOOL ReadClusters(void* buf, DWORD clusters, LONGLONG lcn);
  BOOL ReadVirtualClusters(ULONGLONG vcn, DWORD clusters, void* bufv,
                           DWORD bufLen, DWORD* actural);

 protected:
  virtual BOOL IsDataRunOK() const;

 public:
  virtual ULONGLONG GetDataSize(ULONGLONG* allocSize = NULL) const;
  virtual BOOL ReadData(const ULONGLONG& offset, void* bufv, DWORD bufLen,
                        DWORD* actural) const;
};  // AttrNonResident
}  // namespace NtfsBrowser