#include "attr-index-alloc.h"
#include "data/index-block.h"
#include "data/run-entry.h"
#include "index-block.h"
#include "ntfs-common.h"
#include "flag/index-entry.h"
#include "data/index-entry.h"

namespace NtfsBrowser
{

AttrIndexAlloc::AttrIndexAlloc(const AttrHeaderCommon& ahc,
                               const FileRecord& fr)
    : AttrNonResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Index Allocation\n");

  IndexBlockCount = 0;

  if (IsDataRunOK())
  {
    // Get total number of Index Blocks
    ULONGLONG ibTotalSize;
    ibTotalSize = GetDataSize();
    if (ibTotalSize % GetIndexBlockSize())
    {
      NTFS_TRACE2(
          "Cannot calulate number of IndexBlocks, total size = %I64u, unit = "
          "%u\n",
          ibTotalSize, GetIndexBlockSize());
      return;
    }
    IndexBlockCount = ibTotalSize / GetIndexBlockSize();
  }
  else
  {
    NTFS_TRACE("Index Allocation DataRun parse error\n");
  }
}

AttrIndexAlloc::~AttrIndexAlloc() { NTFS_TRACE("AttrIndexAlloc deleted\n"); }

// Verify US and update sectors
BOOL AttrIndexAlloc::PatchUS(WORD* sector, int sectors, WORD usn, WORD* usarray)
{
  int i;

  for (i = 0; i < sectors; i++)
  {
    sector += ((GetSectorSize() >> 1) - 1);
    if (*sector != usn) return FALSE;  // USN error
    *sector = usarray[i];              // Write back correct data
    sector++;
  }
  return TRUE;
}

ULONGLONG AttrIndexAlloc::GetIndexBlockCount() { return IndexBlockCount; }

// Parse a single Index Block
// vcn = Index Block VCN in Index Allocation Data Attributes
// ibClass holds the parsed Index Entries
BOOL AttrIndexAlloc::ParseIndexBlock(const ULONGLONG& vcn, IndexBlock& ibClass)
{
  if (vcn >= IndexBlockCount)  // Bounds check
    return FALSE;

  // Allocate buffer for a single Index Block
  Data::IndexBlock* ibBuf = ibClass.AllocIndexBlock(GetIndexBlockSize());

  // Sectors Per Index Block
  DWORD sectors = GetIndexBlockSize() / GetSectorSize();

  // Read one Index Block
  DWORD len;
  if (ReadData(vcn * GetIndexBlockSize(), ibBuf, GetIndexBlockSize(), len) &&
      len == GetIndexBlockSize())
  {
    if (ibBuf->Magic != INDEX_BLOCK_MAGIC)
    {
      NTFS_TRACE("Index Block parse error: Magic mismatch\n");
      return FALSE;
    }

    // Patch US
    WORD* usnaddr = (WORD*)((BYTE*)ibBuf + ibBuf->OffsetOfUS);
    WORD usn = *usnaddr;
    WORD* usarray = usnaddr + 1;
    if (!PatchUS((WORD*)ibBuf, sectors, usn, usarray))
    {
      NTFS_TRACE("Index Block parse error: Update Sequence Number\n");
      return FALSE;
    }

    Data::IndexEntry* ie;
    ie = (Data::IndexEntry*)((BYTE*)(&(ibBuf->entry_offset)) +
                             ibBuf->entry_offset);

    DWORD ieTotal = ie->size;

    while (ieTotal <= ibBuf->total_entry_size)
    {
      ibClass.emplace_back(ie);

      if (static_cast<BOOL>(ie->flags & Flag::IndexEntry::LAST))
      {
        NTFS_TRACE("Last Index Entry\n");
        break;
      }

      ie = (Data::IndexEntry*)((BYTE*)ie + ie->size);  // Pick next
      ieTotal += ie->size;
    }

    return TRUE;
  }
  else
    return FALSE;
}

}  // namespace NtfsBrowser