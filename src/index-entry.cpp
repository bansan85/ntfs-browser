#include <ntfs-browser/index-entry.h>
#include "data/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
IndexEntry::IndexEntry(const Data::IndexEntry* ie)
{
  NTFS_TRACE("Index Entry\n");

  IsDefault = FALSE;

  _ASSERT(ie);
  index_entry_ = ie;

  if (IsSubNodePtr())
  {
    NTFS_TRACE("Points to sub-node\n");
  }

  if (ie->StreamSize)
  {
    SetFilename((Attr::Filename*)(ie->Stream));
  }
  else
  {
    NTFS_TRACE("No Filename stream found\n");
  }
}

IndexEntry::~IndexEntry()
{
  // Never touch *index_entry_ here if IsCopy == FALSE !
  // As the memory have been deallocated by ~IndexBlock()

  if (IsCopy && index_entry_) delete (void*)index_entry_;

  NTFS_TRACE("index_entry_ deleted\n");
}

IndexEntry& IndexEntry::operator=(const IndexEntry& ieClass)
{
  if (!IsDefault)
  {
    NTFS_TRACE("Cannot call this routine\n");
    return *this;
  }

  NTFS_TRACE("Index Entry Copied\n");

  IsCopy = TRUE;

  if (index_entry_)
  {
    delete (void*)index_entry_;
    index_entry_ = nullptr;
  }

  const Data::IndexEntry* ie = ieClass.index_entry_;
  _ASSERT(ie && (ie->Size > 0));

  index_entry_ = (Data::IndexEntry*)new BYTE[ie->Size];
  memcpy((void*)index_entry_, ie, ie->Size);
  CopyFilename(&ieClass, (Attr::Filename*)(index_entry_->Stream));

  return *this;
}

ULONGLONG IndexEntry::GetFileReference() const
{
  if (index_entry_)
    return index_entry_->FileReference & 0x0000FFFFFFFFFFFFUL;
  else
    return (ULONGLONG)-1;
}

BOOL IndexEntry::IsSubNodePtr() const
{
  if (index_entry_)
    return static_cast<BOOL>(index_entry_->Flags & Flag::IndexEntry::SUBNODE);
  else
    return FALSE;
}

ULONGLONG IndexEntry::GetSubNodeVCN() const
{
  if (index_entry_)
    return *(ULONGLONG*)((BYTE*)index_entry_ + index_entry_->Size - 8);
  else
    return (ULONGLONG)-1;
}

}  // namespace NtfsBrowser
