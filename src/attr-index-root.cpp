#include <ntfs-browser/data/attr-type.h>

#include "attr-index-root.h"
#include "attr/index-root.h"
#include "data/index-entry.h"
#include "flag/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT>
AttrIndexRoot<RESIDENT>::AttrIndexRoot(const AttrHeaderCommon& ahc,
                                       const FileRecord& fr)
    : RESIDENT(ahc, fr),
      index_root_(reinterpret_cast<const Attr::IndexRoot*>(this->GetData()))
{
  NTFS_TRACE("Attribute: Index Root\n");

  if (!IsFileName())
  {
    NTFS_TRACE("Index View not supported\n");
    return;
  }

  ParseIndexEntries();
}

template <typename RESIDENT>
AttrIndexRoot<RESIDENT>::~AttrIndexRoot()
{
  NTFS_TRACE("AttrIndexRoot deleted\n");
}

// Get all the index entries
template <typename RESIDENT>
void AttrIndexRoot<RESIDENT>::ParseIndexEntries()
{
  const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
      reinterpret_cast<const BYTE*>(&(index_root_->entry_offset)) +
      index_root_->entry_offset);

  DWORD ieTotal = ie->size;

  while (ieTotal <= index_root_->total_entry_size)
  {
    emplace_back(nullptr, *ie);

    if ((ie->flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      NTFS_TRACE("Last Index Entry\n");
      break;
    }

    ie = reinterpret_cast<const Data::IndexEntry*>(
        reinterpret_cast<const BYTE*>(ie) + ie->size);  // Pick next
    ieTotal += ie->size;
  }
}

// Check if this IndexRoot contains Filename or IndexView
template <typename RESIDENT>
bool AttrIndexRoot<RESIDENT>::IsFileName() const noexcept
{
  return index_root_->attr_type == AttrType::FILE_NAME;
}

template class AttrIndexRoot<AttrResidentHeavy>;
template class AttrIndexRoot<AttrResidentLight>;

}  // namespace NtfsBrowser