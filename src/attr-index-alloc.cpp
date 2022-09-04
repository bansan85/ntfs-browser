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
    : AttrNonResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Index Allocation\n");

  if (!IsDataRunOK())
  {
    NTFS_TRACE("Index Allocation DataRun parse error\n");
    return;
  }

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
      return false;
    }
    // Write back correct data
    *sector = usarray[i];
    sector++;
  }

  return true;
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
  // Bounds check
  if (vcn >= index_block_count_)
  {
    return false;
  }

  // Allocate buffer for a single Index Block
  std::shared_ptr<BYTE[]> ib_sh_ptr =
      ibClass.AllocIndexBlock(GetIndexBlockSize());
  Data::IndexBlock* ibBuf =
      reinterpret_cast<Data::IndexBlock*>(&ib_sh_ptr.get()[0]);

  // Sectors Per Index Block
  const DWORD sectors = GetIndexBlockSize() / GetSectorSize();

  // Read one Index Block
  std::optional<ULONGLONG> len =
      ReadData(vcn * GetIndexBlockSize(),
               {reinterpret_cast<BYTE*>(ibBuf), GetIndexBlockSize()});
  if (!len || *len != GetIndexBlockSize())
  {
    return false;
  }

  if (ibBuf->magic != kIndexBlockMagic)
  {
    NTFS_TRACE("Index Block parse error: Magic mismatch\n");
    return false;
  }

  // Patch US
  const auto* usnaddr = reinterpret_cast<const WORD*>(
      reinterpret_cast<const BYTE*>(ibBuf) + ibBuf->offset_of_us);
  const WORD usn = *usnaddr;
  const WORD* usarray = usnaddr + 1;
  if (!PatchUS(reinterpret_cast<WORD*>(ibBuf), sectors, usn, usarray))
  {
    NTFS_TRACE("Index Block parse error: Update Sequence Number\n");
    return false;
  }

  const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
      reinterpret_cast<const BYTE*>(&(ibBuf->entry_offset)) +
      ibBuf->entry_offset);

  DWORD ieTotal = ie->size;

  while (ieTotal <= ibBuf->total_entry_size)
  {
    ibClass.emplace_back(ib_sh_ptr, *ie);

    if ((ie->flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      NTFS_TRACE("Last Index Entry\n");
      break;
    }

    ie = reinterpret_cast<const Data::IndexEntry*>(
        reinterpret_cast<const BYTE*>(ie) + ie->size);  // Pick next
    ieTotal += ie->size;
  }

  return true;
}

}  // namespace NtfsBrowser