#include <ntfs-browser/index-entry.h>
#include "data/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
IndexEntry::IndexEntry(const Data::IndexEntry* ie)
{
  NTFS_TRACE("Index Entry\n");

  is_default_ = FALSE;

  _ASSERT(ie);
  index_entry_ = ie;

  if (IsSubNodePtr())
  {
    NTFS_TRACE("Points to sub-node\n");
  }

  if (ie->stream_size)
  {
    SetFilename(*(const Attr::Filename*)(&ie->stream));
  }
  else
  {
    NTFS_TRACE("No Filename stream found\n");
  }
}

IndexEntry& IndexEntry::operator=(const IndexEntry& ieClass)
{
  if (!is_default_)
  {
    NTFS_TRACE("Cannot call this routine\n");
    return *this;
  }

  NTFS_TRACE("Index Entry Copied\n");

  if (index_entry_)
  {
    delete (void*)index_entry_;
    index_entry_ = nullptr;
  }

  const Data::IndexEntry* ie = ieClass.index_entry_;
  _ASSERT(ie && (ie->size > 0));

  index_entry_ = (Data::IndexEntry*)new BYTE[ie->size];
  memcpy((void*)index_entry_, ie, ie->size);
  CopyFilename(ieClass, *(const Attr::Filename*)(&index_entry_->stream));

  return *this;
}

ULONGLONG IndexEntry::GetFileReference() const
{
  if (index_entry_)
    return index_entry_->file_reference & 0x0000FFFFFFFFFFFFUL;
  else
    return (ULONGLONG)-1;
}

bool IndexEntry::IsSubNodePtr() const
{
  if (index_entry_)
    return static_cast<bool>(index_entry_->flags & Flag::IndexEntry::SUBNODE);
  else
    return FALSE;
}

ULONGLONG IndexEntry::GetSubNodeVCN() const
{
  if (index_entry_)
    return *(ULONGLONG*)((BYTE*)index_entry_ + index_entry_->size - 8);
  else
    return (ULONGLONG)-1;
}

}  // namespace NtfsBrowser
