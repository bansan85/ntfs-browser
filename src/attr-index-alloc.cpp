#include "attr-index-alloc.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "flag/index-entry.h"
#include "index-block.h"
#include "ntfs-browser/win-types.h"
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
bool AttrIndexAlloc<S>::PatchUS(std::span<WORD> block, DWORD sectors, WORD usn,
                                std::span<const WORD> usarray)
{
  for (DWORD i = 0; i < sectors; i++)
  {
    // The last word of the i-th sector holds the USN.
    const size_t pos = ((i + 1) * (kUpdateSequenceStride / sizeof(WORD))) - 1;
    // USN error
    if (pos >= block.size() || block[pos] != usn)
    {
      return false;
    }
    // Write back correct data
    block[pos] = usarray[i];
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
  std::shared_ptr<BYTE[]> const ib_sh_ptr =
      ibClass.AllocIndexBlock(this->GetIndexBlockSize());
  const std::span<BYTE> block(ib_sh_ptr.get(), this->GetIndexBlockSize());
  Data::IndexBlock* ibBuf = reinterpret_cast<Data::IndexBlock*>(block.data());

  // Read one Index Block
  std::optional<ULONGLONG> len = this->ReadData(byte_offset, block);
  if (!len || *len != this->GetIndexBlockSize())
  {
    return false;
  }

  if (ibBuf->magic != kIndexBlockMagic)
  {
    LogWarn("Index Block parse error: Magic mismatch");
    return false;
  }

  const DWORD sectors = gsl::narrow<DWORD>(
      UpdateSequenceBlockCount(this->GetIndexBlockSize(), ibBuf->size_of_us));
  if (!IndexBlockUsOffsetInBounds(ibBuf->offset_of_us, sectors,
                                  this->GetIndexBlockSize()))
  {
    LogWarn("Index Block parse error: offset_of_us out of bounds");
    return false;
  }

  // Patch US
  // offset_of_us is not checked for alignment, so read the words as bytes.
  const std::span<const BYTE> usnArea = block.subspan(ibBuf->offset_of_us);
  WORD usn = 0;
  std::memcpy(&usn, &usnArea[0], sizeof(usn));
  std::vector<WORD> usarray(sectors);
  for (DWORD i = 0; i < sectors; i++)
  {
    std::memcpy(&usarray[i], &usnArea[sizeof(usn) + (i * sizeof(WORD))],
                sizeof(WORD));
  }
  if (!PatchUS(
          {reinterpret_cast<WORD*>(block.data()), block.size() / sizeof(WORD)},
          sectors, usn, usarray))
  {
    LogWarn("Index Block parse error: Update Sequence Number");
    return false;
  }

  constexpr size_t kEntryOffsetPos = offsetof(Data::IndexBlock, entry_offset);

  if (block.size() < kEntryOffsetPos ||
      ibBuf->entry_offset > block.size() - kEntryOffsetPos)
  {
    LogWarn("Index Block: entry_offset exceeds block bounds");
    return false;
  }

  const bool recover = this->volume_.GetOptions().recover_errors;
  // An entry's position comes from the disk, so it need not be aligned.
  std::span<const BYTE> cur = std::span<const BYTE>(block)
                                  .subspan(kEntryOffsetPos)
                                  .subspan(ibBuf->entry_offset);
  DWORD ieTotal = 0;

  while (true)
  {
    const size_t remaining = cur.size();
    if (remaining < offsetof(Data::IndexEntry, stream))
    {
      LogRecoverable(recover,
                     "Index Block: index entry header exceeds block bounds");
      if (!recover)
      {
        ibClass.clear();
        return false;
      }
      break;
    }
    const Data::IndexEntry head = ReadIndexEntryHeader(cur);
    if (head.size == 0 || head.size > remaining)
    {
      LogRecoverable(recover, "Index Block: index entry exceeds block bounds");
      if (!recover)
      {
        ibClass.clear();
        return false;
      }
      break;
    }

    ieTotal += head.size;
    if (ieTotal > ibBuf->total_entry_size)
    {
      LogRecoverable(recover,
                     "Index Block: index entry total exceeds the block's "
                     "declared entry size");
      if (!recover)
      {
        ibClass.clear();
        return false;
      }
      break;
    }

    const AlignedIndexEntry ie = AlignIndexEntry(ib_sh_ptr, cur, head.size);
    if (const std::optional<std::string_view> defect =
            ValidateIndexEntry(*ie.entry))
    {
      LogRecoverable(recover, "{}", *defect);
      if (!recover)
      {
        ibClass.clear();
        return false;
      }
    }

    ibClass.emplace_back(ie.owner, *ie.entry);

    if ((head.flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      LogTrace("Last Index Entry");
      break;
    }

    cur = cur.subspan(head.size);  // Pick next
  }

  return true;
}

template class AttrIndexAlloc<Strategy::FULL_CACHE>;
template class AttrIndexAlloc<Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
