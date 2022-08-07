#include "attr-index-root.h"
#include <ntfs-browser/data/attr-type.h>
#include "flag/index-entry.h"
#include "data/index-entry.h"
#include "ntfs-common.h"
#include "attr/index-root.h"

namespace NtfsBrowser
{

AttrIndexRoot::AttrIndexRoot(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Index Root\n");

  IndexRoot = (Attr::IndexRoot*)GetData();

  if (IsFileName())
  {
    ParseIndexEntries();
  }
  else
  {
    NTFS_TRACE("Index View not supported\n");
  }
}

AttrIndexRoot::~AttrIndexRoot() { NTFS_TRACE("AttrIndexRoot deleted\n"); }

// Get all the index entries
void AttrIndexRoot::ParseIndexEntries()
{
  Data::IndexEntry* ie;
  ie = (Data::IndexEntry*)((BYTE*)(&(IndexRoot->EntryOffset)) +
                           IndexRoot->EntryOffset);

  DWORD ieTotal = ie->Size;

  while (ieTotal <= IndexRoot->TotalEntrySize)
  {
    emplace_back(ie);

    if (static_cast<BOOL>(ie->Flags & Flag::IndexEntry::LAST))
    {
      NTFS_TRACE("Last Index Entry\n");
      break;
    }

    ie = (Data::IndexEntry*)((BYTE*)ie + ie->Size);  // Pick next
    ieTotal += ie->Size;
  }
}

// Check if this IndexRoot contains Filename or IndexView
BOOL AttrIndexRoot::IsFileName() const
{
  return (IndexRoot->attr_type == static_cast<DWORD>(AttrType::FILE_NAME));
}

}  // namespace NtfsBrowser