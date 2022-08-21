#include <ntfs-browser/index-entry.h>

#include "data/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
IndexEntry::IndexEntry(std::optional<std::shared_ptr<BYTE[]>> sh_ptr,
                       const Data::IndexEntry& ie)
    : sh_ptr_(sh_ptr), index_entry_(ie)
{
  NTFS_TRACE("Index Entry\n");

  if (IsSubNodePtr())
  {
    NTFS_TRACE("Points to sub-node\n");
  }

  if (ie.stream_size == 0)
  {
    NTFS_TRACE("No Filename stream found\n");
    return;
  }

  SetFilename(*reinterpret_cast<const Attr::Filename*>(&ie.stream));
}

ULONGLONG IndexEntry::GetFileReference() const noexcept
{
  return index_entry_.mft_index;
}

bool IndexEntry::IsSubNodePtr() const noexcept
{
  return (index_entry_.flags & Flag::IndexEntry::SUBNODE) ==
         Flag::IndexEntry::SUBNODE;
}

ULONGLONG IndexEntry::GetSubNodeVCN() const noexcept
{
  return *reinterpret_cast<const ULONGLONG*>(
      reinterpret_cast<const BYTE*>(&index_entry_) + index_entry_.size - 8);
}

}  // namespace NtfsBrowser
