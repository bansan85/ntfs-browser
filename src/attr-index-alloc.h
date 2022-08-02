#pragma once

#include "attr-non-resident.h"

namespace NtfsBrowser
{
class IndexBlock;
class FileRecord;
struct AttrHeaderCommon;

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