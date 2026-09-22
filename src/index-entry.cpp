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
  LogTrace("Index Entry");

  if (IsSubNodePtr())
  {
    LogTrace("Points to sub-node");
  }

  if (ie.stream_size == 0)
  {
    LogInfo("No Filename stream found");
    return;
  }

  // stream_size has no guaranteed relation to ie.size; derive room for the
  // stream from ie.size instead.
  const size_t stream_offset = offsetof(Data::IndexEntry, stream);
  if (ie.size <= stream_offset)
  {
    LogWarn("Index Entry stream exceeds entry bounds");
    return;
  }
  const size_t available = ie.size - stream_offset;
  if (available < offsetof(Attr::Filename, name))
  {
    LogWarn("Index Entry stream smaller than expected");
    return;
  }

  const auto& fn = *reinterpret_cast<const Attr::Filename*>(&ie.stream);
  if (available < offsetof(Attr::Filename, name) +
                      (static_cast<size_t>(fn.name_length) * sizeof(WORD)))
  {
    LogWarn("Index Entry Filename name exceeds entry bounds");
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
