#pragma once

#include <ntfs-browser/file-name.h>
#include <ntfs-browser/ntfs-common.h>
#include <ntfs-browser/data/index-entry.h>
#include <ntfs-browser/data/attr-filename.h>
#include <ntfs-browser/data/index-entry-flag.h>

namespace NtfsBrowser
{

/////////////////////////////
// Index Entry helper class
/////////////////////////////
class IndexEntry : public FileName
{
 public:
  IndexEntry()
  {
    NTFS_TRACE("Index Entry\n");

    IsDefault = TRUE;

    index_entry_ = NULL;
    SetFileName(NULL);
  }

  IndexEntry(const Data::IndexEntry* ie)
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
      SetFileName((Data::AttrFilename*)(ie->Stream));
    }
    else
    {
      NTFS_TRACE("No FileName stream found\n");
    }
  }

  virtual ~IndexEntry()
  {
    // Never touch *index_entry_ here if IsCopy == FALSE !
    // As the memory have been deallocated by ~IndexBlock()

    if (IsCopy && index_entry_) delete (void*)index_entry_;

    NTFS_TRACE("index_entry_ deleted\n");
  }

 private:
  BOOL IsDefault;

 protected:
  const Data::IndexEntry* index_entry_;

 public:
  // Use with caution !
  IndexEntry& operator=(const IndexEntry& ieClass)
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
      index_entry_ = NULL;
    }

    const Data::IndexEntry* ie = ieClass.index_entry_;
    _ASSERT(ie && (ie->Size > 0));

    index_entry_ = (Data::IndexEntry*)new BYTE[ie->Size];
    memcpy((void*)index_entry_, ie, ie->Size);
    CopyFileName(&ieClass, (Data::AttrFilename*)(index_entry_->Stream));

    return *this;
  }

  ULONGLONG GetFileReference() const
  {
    if (index_entry_)
      return index_entry_->FileReference & 0x0000FFFFFFFFFFFFUL;
    else
      return (ULONGLONG)-1;
  }

  BOOL IsSubNodePtr() const
  {
    if (index_entry_)
      return (index_entry_->Flags & static_cast<BYTE>(IndexEntryFlag::SUBNODE));
    else
      return FALSE;
  }

  ULONGLONG GetSubNodeVCN() const
  {
    if (index_entry_)
      return *(ULONGLONG*)((BYTE*)index_entry_ + index_entry_->Size - 8);
    else
      return (ULONGLONG)-1;
  }
};  // IndexEntry

}  // namespace NtfsBrowser