#include <ntfs-browser/attr-index-root.h>
#include <ntfs-browser/data/index-entry-flag.h>
#include <ntfs-browser/data/attr-type.h>

namespace NtfsBrowser
{

AttrIndexRoot::AttrIndexRoot(const AttrHeaderCommon* ahc, const FileRecord* fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: Index Root\n");

  IndexRoot = (Data::AttrIndexRoot*)AttrBody;

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
    IndexEntry* ieClass = new IndexEntry(ie);
    push_back(ieClass);

    if (ie->Flags & static_cast<BYTE>(IndexEntryFlag::LAST))
    {
      NTFS_TRACE("Last Index Entry\n");
      break;
    }

    ie = (Data::IndexEntry*)((BYTE*)ie + ie->Size);  // Pick next
    ieTotal += ie->Size;
  }
}

// Check if this IndexRoot contains FileName or IndexView
BOOL AttrIndexRoot::IsFileName() const
{
  return (IndexRoot->AttrType == static_cast<DWORD>(AttrType::FILE_NAME));
}

}  // namespace NtfsBrowser