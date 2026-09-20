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

  LogTrace("Attribute: Index Root");

  if (!IsFileName())
  {
    LogWarn("Index View not supported");
    return;
  }

  ParseIndexEntries();
}

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::~AttrIndexRoot()
{
  LogTrace("AttrIndexRoot deleted");
}

// Parses every index entry, bounding each step against the resident
// attribute's own size. Every returned IndexEntry keeps its own copy of
// the backing bytes alive, independent of this object's lifetime.
template <typename RESIDENT, Strategy S>
void AttrIndexRoot<RESIDENT, S>::ParseIndexEntries()
{
  const ULONGLONG data_size = this->GetDataSize();
  const auto data_copy = std::make_shared<BYTE[]>(data_size);
  std::memcpy(data_copy.get(), this->GetData(), data_size);
  LogDebug("Index Root: allocated independent copy of resident data");

  const BYTE* const data_end = data_copy.get() + data_size;
  const auto* const index_root_copy =
      reinterpret_cast<const Attr::IndexRoot*>(data_copy.get());
  const auto* const entry_offset_addr =
      reinterpret_cast<const BYTE*>(&(index_root_copy->entry_offset));

  if (index_root_copy->entry_offset >
      static_cast<ULONGLONG>(data_end - entry_offset_addr))
  {
    LogWarn("Index Root: entry_offset exceeds attribute bounds");
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
      LogWarn("Index Root: index entry header exceeds attribute bounds");
      break;
    }
    if (ie->size == 0 ||
        reinterpret_cast<const BYTE*>(ie) + ie->size > data_end)
    {
      LogWarn("Index Root: index entry exceeds attribute bounds");
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
      LogTrace("Last Index Entry");
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