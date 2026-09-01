#include <cstddef>

#include <ntfs-browser/index-entry.h>

#include "attr/filename.h"
#include "data/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
IndexEntry::IndexEntry(std::shared_ptr<BYTE[]> sh_ptr,
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

  // ie.size was already bounds-checked by the caller against its containing
  // buffer; stream_size is a separate on-disk field with no guaranteed
  // relation to it, so re-derive how much room is actually left for the
  // stream before trusting it.
  const size_t stream_offset = offsetof(Data::IndexEntry, stream);
  if (ie.size <= stream_offset)
  {
    NTFS_TRACE("Index Entry stream exceeds entry bounds\n");
    return;
  }
  const size_t available = ie.size - stream_offset;
  if (available < offsetof(Attr::Filename, name))
  {
    NTFS_TRACE("Index Entry stream smaller than expected\n");
    return;
  }

  const auto& fn = *reinterpret_cast<const Attr::Filename*>(&ie.stream);
  if (available < offsetof(Attr::Filename, name) +
                      (static_cast<size_t>(fn.name_length) * sizeof(WORD)))
  {
    NTFS_TRACE("Index Entry Filename name exceeds entry bounds\n");
    return;
  }

  SetFilename(fn);
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
