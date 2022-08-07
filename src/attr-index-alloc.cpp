#include "attr-index-alloc.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/run-entry.h"
#include "flag/index-entry.h"
#include "index-block.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrIndexAlloc::AttrIndexAlloc(const AttrHeaderCommon& ahc,
                               const FileRecord& fr)
    : AttrNonResident(ahc, fr), index_block_count_(0)
{
  NTFS_TRACE("Attribute: Index Allocation\n");

  if (IsDataRunOK())
  {
    // Get total number of Index Blocks
    const ULONGLONG ibTotalSize = GetDataSize();
    if (ibTotalSize % GetIndexBlockSize() != 0)
    {
      NTFS_TRACE2(
          "Cannot calulate number of IndexBlocks, total size = %I64u, unit = "
          "%u\n",
          ibTotalSize, GetIndexBlockSize());
      return;
    }
    index_block_count_ = ibTotalSize / GetIndexBlockSize();
  }
  else
  {
    NTFS_TRACE("Index Allocation DataRun parse error\n");
  }
}

AttrIndexAlloc::~AttrIndexAlloc() { NTFS_TRACE("AttrIndexAlloc deleted\n"); }

// Verify US and update sectors
bool AttrIndexAlloc::PatchUS(WORD* sector, DWORD sectors, WORD usn,
                             const WORD* usarray)
{
  for (DWORD i = 0; i < sectors; i++)
  {
    sector += GetSectorSize() / 2;
    sector--;
    // USN error
    if (*sector != usn)
    {
      return FALSE;
    }
    // Write back correct data
    *sector = usarray[i];
    sector++;
  }
  return TRUE;
}

ULONGLONG AttrIndexAlloc::GetIndexBlockCount() const noexcept
{
  return index_block_count_;
}

// Parse a single Index Block
// vcn = Index Block VCN in Index Allocation Data Attributes
// ibClass holds the parsed Index Entries
bool AttrIndexAlloc::ParseIndexBlock(const ULONGLONG& vcn, IndexBlock& ibClass)
{
  if (vcn >= index_block_count_)  // Bounds check
  {
    return FALSE;
  }

  // Allocate buffer for a single Index Block
  Data::IndexBlock* ibBuf = ibClass.AllocIndexBlock(GetIndexBlockSize());

  // Sectors Per Index Block
  const DWORD sectors = GetIndexBlockSize() / GetSectorSize();

  // Read one Index Block
  DWORD len = 0;
  if (ReadData(vcn * GetIndexBlockSize(), ibBuf, GetIndexBlockSize(), len) &&
      len == GetIndexBlockSize())
  {
    if (ibBuf->magic != kIndexBlockMagic)
    {
      NTFS_TRACE("Index Block parse error: Magic mismatch\n");
      return FALSE;
    }

    // Patch US
    const auto* usnaddr = reinterpret_cast<const WORD*>(
        reinterpret_cast<const BYTE*>(ibBuf) + ibBuf->offset_of_us);
    const WORD usn = *usnaddr;
    const WORD* usarray = usnaddr + 1;
    if (!PatchUS(reinterpret_cast<WORD*>(ibBuf), sectors, usn, usarray))
    {
      NTFS_TRACE("Index Block parse error: Update Sequence Number\n");
      return FALSE;
    }

    const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
        reinterpret_cast<const BYTE*>(&(ibBuf->entry_offset)) +
        ibBuf->entry_offset);

    DWORD ieTotal = ie->size;

    while (ieTotal <= ibBuf->total_entry_size)
    {
      ibClass.emplace_back(ie);

      if (static_cast<bool>(ie->flags & Flag::IndexEntry::LAST))
      {
        NTFS_TRACE("Last Index Entry\n");
        break;
      }

      ie = reinterpret_cast<const Data::IndexEntry*>(
          reinterpret_cast<const BYTE*>(ie) + ie->size);  // Pick next
      ieTotal += ie->size;
    }

    return TRUE;
  }

  return FALSE;
}

}  // namespace NtfsBrowser