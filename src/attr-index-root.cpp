#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <ntfs-browser/data/attr-type.h>

#include "attr-index-root.h"
#include "attr/index-root.h"
#include "data/index-entry.h"
#include "flag/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::AttrIndexRoot(const AttrHeaderCommon& ahc,
                                          const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
      index_root_(reinterpret_cast<const Attr::IndexRoot*>(this->GetData()))
{
  if (this->GetDataSize() < sizeof(Attr::IndexRoot))
  {
    throw std::runtime_error("Index Root attribute smaller than expected.\n");
  }

  NTFS_TRACE("Attribute: Index Root\n");

  if (!IsFileName())
  {
    NTFS_TRACE("Index View not supported\n");
    return;
  }

  ParseIndexEntries();
}

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::~AttrIndexRoot()
{
  NTFS_TRACE("AttrIndexRoot deleted\n");
}

// Get all the index entries. entry_offset/total_entry_size and each entry's
// own "size" come straight off disk -- none of them are otherwise related to
// GetDataSize(), so every step here is bounded against data_end to avoid
// walking past the resident attribute buffer.
//
// Every IndexEntry this method hands out is kept alive by data_copy, an
// independent heap copy of this resident attribute's bytes - NOT by
// this->GetData() (FileRecord::record_buffer_ under NO_CACHE, or this
// object's own AttrResidentFullCache::body_ under FULL_CACHE), whose
// lifetime is tied to the FileRecord/AttrIndexRoot object itself. Mirrors
// AttrIndexAlloc<S>::ParseIndexBlock() (src/attr-index-alloc.cpp), whose
// entries already own their backing Index Block buffer this same way - see
// F21 in docs/bug-reports/2026-09-03-full-repo.md.
template <typename RESIDENT, Strategy S>
void AttrIndexRoot<RESIDENT, S>::ParseIndexEntries()
{
  const ULONGLONG data_size = this->GetDataSize();
  const auto data_copy = std::make_shared<BYTE[]>(data_size);
  std::memcpy(data_copy.get(), this->GetData(), data_size);
  NTFS_TRACE("Index Root: allocated independent copy of resident data\n");

  const BYTE* const data_end = data_copy.get() + data_size;
  const auto* const index_root_copy =
      reinterpret_cast<const Attr::IndexRoot*>(data_copy.get());
  const auto* const entry_offset_addr =
      reinterpret_cast<const BYTE*>(&(index_root_copy->entry_offset));

  if (index_root_copy->entry_offset >
      static_cast<ULONGLONG>(data_end - entry_offset_addr))
  {
    NTFS_TRACE("Index Root: entry_offset exceeds attribute bounds\n");
    return;
  }

  const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
      entry_offset_addr + index_root_copy->entry_offset);
  DWORD ieTotal = 0;

  while (true)
  {
    if (reinterpret_cast<const BYTE*>(ie) + offsetof(Data::IndexEntry, stream) >
        data_end)
    {
      NTFS_TRACE("Index Root: index entry header exceeds attribute bounds\n");
      break;
    }
    if (ie->size == 0 ||
        reinterpret_cast<const BYTE*>(ie) + ie->size > data_end)
    {
      NTFS_TRACE("Index Root: index entry exceeds attribute bounds\n");
      break;
    }

    ieTotal += ie->size;
    if (ieTotal > index_root_copy->total_entry_size)
    {
      break;
    }

    emplace_back(data_copy, *ie);

    if ((ie->flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      NTFS_TRACE("Last Index Entry\n");
      break;
    }

    ie = reinterpret_cast<const Data::IndexEntry*>(
        reinterpret_cast<const BYTE*>(ie) + ie->size);  // Pick next
  }
}

// Check if this IndexRoot contains Filename or IndexView
template <typename RESIDENT, Strategy S>
bool AttrIndexRoot<RESIDENT, S>::IsFileName() const noexcept
{
  return index_root_->attr_type == AttrType::FILE_NAME;
}

template class AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser