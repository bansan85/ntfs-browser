#include <ntfs-browser/attr-index-alloc.h>

namespace NtfsBrowser
{

AttrIndexAlloc::AttrIndexAlloc(const AttrHeaderCommon* ahc,
                               const FileRecord* fr)
    : AttrNonResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Index Allocation\n");

  IndexBlockCount = 0;

  if (IsDataRunOK())
  {
    // Get total number of Index Blocks
    ULONGLONG ibTotalSize;
    ibTotalSize = GetDataSize();
    if (ibTotalSize % _IndexBlockSize)
    {
      NTFS_TRACE2(
          "Cannot calulate number of IndexBlocks, total size = %I64u, unit = "
          "%u\n",
          ibTotalSize, _IndexBlockSize);
      return;
    }
    IndexBlockCount = ibTotalSize / _IndexBlockSize;
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
    sector += ((_SectorSize >> 1) - 1);
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
  Data::IndexBlock* ibBuf = ibClass.AllocIndexBlock(_IndexBlockSize);

  // Sectors Per Index Block
  DWORD sectors = _IndexBlockSize / _SectorSize;

  // Read one Index Block
  DWORD len;
  if (ReadData(vcn * _IndexBlockSize, ibBuf, _IndexBlockSize, &len) &&
      len == _IndexBlockSize)
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
    ie = (Data::IndexEntry*)((BYTE*)(&(ibBuf->EntryOffset)) +
                             ibBuf->EntryOffset);

    DWORD ieTotal = ie->Size;

    while (ieTotal <= ibBuf->TotalEntrySize)
    {
      IndexEntry* ieClass = new IndexEntry(ie);
      ibClass.push_back(ieClass);

      if (ie->Flags & static_cast<BYTE>(IndexEntryFlag::LAST))
      {
        NTFS_TRACE("Last Index Entry\n");
        break;
      }

      ie = (Data::IndexEntry*)((BYTE*)ie + ie->Size);  // Pick next
      ieTotal += ie->Size;
    }

    return TRUE;
  }
  else
    return FALSE;
}

}  // namespace NtfsBrowser