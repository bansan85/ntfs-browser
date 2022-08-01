#pragma once

#include <ntfs-browser/attr-non-resident.h>
#include <ntfs-browser/index-block.h>

namespace NtfsBrowser
{

/////////////////////////////////////////////
// Attribute: Index Allocation (NonResident)
/////////////////////////////////////////////
class AttrIndexAlloc : public AttrNonResident
{
 public:
  AttrIndexAlloc(const AttrHeaderCommon* ahc, const FileRecord* fr);
  virtual ~AttrIndexAlloc();

 private:
  ULONGLONG IndexBlockCount;

  BOOL PatchUS(WORD* sector, int sectors, WORD usn, WORD* usarray);

 public:
  ULONGLONG GetIndexBlockCount();
  BOOL ParseIndexBlock(const ULONGLONG& vcn, IndexBlock& ibClass);
};  // AttrIndexAlloc

}  // namespace NtfsBrowser