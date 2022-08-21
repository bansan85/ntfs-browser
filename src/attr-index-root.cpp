#include <ntfs-browser/data/attr-type.h>

#include "attr-index-root.h"
#include "attr/index-root.h"
#include "data/index-entry.h"
#include "flag/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrIndexRoot::AttrIndexRoot(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr),
      index_root_(reinterpret_cast<const Attr::IndexRoot*>(GetData()))
{
  NTFS_TRACE("Attribute: Index Root\n");

  if (!IsFileName())
  {
    NTFS_TRACE("Index View not supported\n");
    return;
  }

  ParseIndexEntries();
}

AttrIndexRoot::~AttrIndexRoot() { NTFS_TRACE("AttrIndexRoot deleted\n"); }

// Get all the index entries
void AttrIndexRoot::ParseIndexEntries()
{
  const auto* ie = reinterpret_cast<const Data::IndexEntry*>(
      reinterpret_cast<const BYTE*>(&(index_root_->entry_offset)) +
      index_root_->entry_offset);

  DWORD ieTotal = ie->size;

  while (ieTotal <= index_root_->total_entry_size)
  {
    emplace_back(std::nullopt, *ie);

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
bool AttrIndexRoot::IsFileName() const noexcept
{
  return (index_root_->attr_type == static_cast<DWORD>(AttrType::FILE_NAME));
}

}  // namespace NtfsBrowser