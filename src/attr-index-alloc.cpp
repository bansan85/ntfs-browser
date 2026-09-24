#include "attr-index-alloc.h"

#include <cstddef>
#include <limits>

#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/run-entry.h"
#include "flag/index-entry.h"
#include "index-block.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

bool IndexBlockUsOffsetInBounds(WORD offset_of_us, DWORD sectors,
                                DWORD index_block_size) noexcept
{
  // True only if offset_of_us starts past the header and the whole USN
  // array still fits within the buffer.
  return offset_of_us >= sizeof(Data::IndexBlock) &&
         static_cast<ULONGLONG>(offset_of_us) + 2ULL * (1ULL + sectors) <=
             index_block_size;
}

template <Strategy S>
AttrIndexAlloc<S>::AttrIndexAlloc(const AttrHeaderCommon& ahc,
                                  const FileRecord<S>& fr)
    : AttrNonResident<S>(ahc, fr)
{
  LogTrace("Attribute: Index Allocation");

  // Get total number of Index Blocks
  const ULONGLONG ibTotalSize = this->GetDataSize();
  if (ibTotalSize % this->GetIndexBlockSize() != 0)
  {
    LogWarn("Cannot calulate number of IndexBlocks, total size = {}, unit = {}",
            ibTotalSize, this->GetIndexBlockSize());
    return;
  }

  index_block_count_ = ibTotalSize / this->GetIndexBlockSize();
}

template <Strategy S>
AttrIndexAlloc<S>::~AttrIndexAlloc()
{
  LogTrace("AttrIndexAlloc deleted");
}

// Verify US and update sectors
template <Strategy S>
bool AttrIndexAlloc<S>::PatchUS(WORD* sector, DWORD sectors, WORD usn,
                                const WORD* usarray)
{
  for (DWORD i = 0; i < sectors; i++)
  {
    sector += this->GetSectorSize() / 2;
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

template <Strategy S>
ULONGLONG AttrIndexAlloc<S>::GetIndexBlockCount() const noexcept
{
  return index_block_count_;
}

// Parse a single Index Block
// vcn = sub-node pointer read from an Index Entry, on-disk units (see below)
// ibClass holds the parsed Index Entries
template <Strategy S>
bool AttrIndexAlloc<S>::ParseIndexBlock(const ULONGLONG& vcn,
                                        IndexBlock& ibClass)
{
  // On disk, a sub-node VCN is in clusters when an index block spans a whole
  // cluster or more, but in index_block_size units when a cluster is too
  // big to hold one (index_block_size then < cluster_size). Converting it
  // to a byte offset needs whichever unit is actually in effect.
  const DWORD vcn_unit = this->GetIndexBlockSize() >= this->GetClusterSize()
                             ? this->GetClusterSize()
                             : this->GetIndexBlockSize();

  // Reject a vcn whose multiply would overflow before it can be compared.
  if (vcn > std::numeric_limits<ULONGLONG>::max() / vcn_unit)
  {
    LogWarn("Index Block: sub-node vcn overflows byte offset");
    return false;
  }

  const ULONGLONG byte_offset = vcn * vcn_unit;

  // Bounds check: the offset must land exactly on one of the stream's
  // index_block_size-sized blocks.
  if (byte_offset % this->GetIndexBlockSize() != 0 ||
      byte_offset / this->GetIndexBlockSize() >= index_block_count_)
  {
    LogWarn("Index Block: sub-node vcn out of bounds");
    return false;
  }

  // Allocate buffer for a single Index Block
  std::shared_ptr<BYTE[]> ib_sh_ptr =
      ibClass.AllocIndexBlock(this->GetIndexBlockSize());
  Data::IndexBlock* ibBuf =
      reinterpret_cast<Data::IndexBlock*>(&ib_sh_ptr.get()[0]);

  // Sectors Per Index Block
  const DWORD sectors = this->GetIndexBlockSize() / this->GetSectorSize();

  // Read one Index Block
  std::optional<ULONGLONG> len = this->ReadData(
      byte_offset,
      {reinterpret_cast<BYTE*>(ibBuf), this->GetIndexBlockSize()});
  if (!len || *len != this->GetIndexBlockSize())
  {
    return false;
  }

  if (ibBuf->magic != kIndexBlockMagic)
  {
    LogWarn("Index Block parse error: Magic mismatch");
    return false;
  }

  if (!IndexBlockUsOffsetInBounds(ibBuf->offset_of_us, sectors,
                                  this->GetIndexBlockSize()))
  {
    LogWarn("Index Block parse error: offset_of_us out of bounds");
    return false;
  }

  // Patch US
  const auto* usnaddr = reinterpret_cast<const WORD*>(
      reinterpret_cast<const BYTE*>(ibBuf) + ibBuf->offset_of_us);
  const WORD usn = *usnaddr;
  const WORD* usarray = usnaddr + 1;
  if (!PatchUS(reinterpret_cast<WORD*>(ibBuf), sectors, usn, usarray))
  {
    LogWarn("Index Block parse error: Update Sequence Number");
    return false;
  }

  const BYTE* const block_end =
      reinterpret_cast<const BYTE*>(ibBuf) + this->GetIndexBlockSize();
  const auto* const entry_offset_addr =
      reinterpret_cast<const BYTE*>(&(ibBuf->entry_offset));

  if (ibBuf->entry_offset >
      static_cast<ULONGLONG>(block_end - entry_offset_addr))
  {
    LogWarn("Index Block: entry_offset exceeds block bounds");
    return false;
  }

  const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
      entry_offset_addr + ibBuf->entry_offset);
  DWORD ieTotal = 0;

  while (true)
  {
    if (reinterpret_cast<const BYTE*>(ie) + offsetof(Data::IndexEntry, stream) >
        block_end)
    {
      LogWarn("Index Block: index entry header exceeds block bounds");
      break;
    }
    if (ie->size == 0 ||
        reinterpret_cast<const BYTE*>(ie) + ie->size > block_end)
    {
      LogWarn("Index Block: index entry exceeds block bounds");
      break;
    }

    ieTotal += ie->size;
    if (ieTotal > ibBuf->total_entry_size)
    {
      break;
    }

    ibClass.emplace_back(ib_sh_ptr, *ie);

    if ((ie->flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      LogTrace("Last Index Entry");
      break;
    }

    ie = reinterpret_cast<const Data::IndexEntry*>(
        reinterpret_cast<const BYTE*>(ie) + ie->size);  // Pick next
  }

  return true;
}

template class AttrIndexAlloc<Strategy::FULL_CACHE>;
template class AttrIndexAlloc<Strategy::NO_CACHE>;

}  // namespace NtfsBrowser