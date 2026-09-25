#include "data/index-entry.h"

#include <cstddef>

#include <ntfs-browser/index-entry.h>

#include "attr/filename.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

std::optional<std::string_view>
    ValidateIndexEntry(const Data::IndexEntry& ie) noexcept
{
  if (ie.stream_size != 0)
  {
    // stream_size has no guaranteed relation to ie.size; derive room for the
    // stream from ie.size instead.
    const size_t stream_offset = offsetof(Data::IndexEntry, stream);
    if (ie.size <= stream_offset)
    {
      return "Index Entry stream exceeds entry bounds";
    }
    const size_t available = ie.size - stream_offset;
    if (available < offsetof(Attr::Filename, name))
    {
      return "Index Entry stream smaller than expected";
    }

    const auto& fn = *reinterpret_cast<const Attr::Filename*>(&ie.stream);
    if (available < offsetof(Attr::Filename, name) +
                        (static_cast<size_t>(fn.name_length) * sizeof(WORD)))
    {
      return "Index Entry Filename name exceeds entry bounds";
    }
  }

  // GetSubNodeVCN() reads 8 bytes at size - 8, unchecked: a SUBNODE entry
  // must have room for that field regardless of whether it also has a name.
  if ((ie.flags & Flag::IndexEntry::SUBNODE) == Flag::IndexEntry::SUBNODE &&
      ie.size < offsetof(Data::IndexEntry, stream) + 8)
  {
    return "Index Entry is a sub-node pointer too small for its VCN field";
  }

  return std::nullopt;
}

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

  // The caller (AttrIndexAlloc/AttrIndexRoot) already validated and logged
  // this same defect, at the level it can act on (strict vs. recovering).
  if (ValidateIndexEntry(ie))
  {
    return;
  }

  SetFilename(*reinterpret_cast<const Attr::Filename*>(&ie.stream));
}

ULONGLONG IndexEntry::GetFileReference() const noexcept
{
  return index_entry_.mft_index;
}

WORD IndexEntry::GetSequenceNumber() const noexcept
{
  return static_cast<WORD>(index_entry_.mft_sn);
}

bool IndexEntry::IsSubNodePtr() const noexcept
{
  // A recovering parse still keeps a too-small SUBNODE entry (matching the
  // matrix's "kept nameless" disposition), but must never let it be treated
  // as a usable sub-node pointer: GetSubNodeVCN() reads unchecked at size-8.
  return (index_entry_.flags & Flag::IndexEntry::SUBNODE) ==
             Flag::IndexEntry::SUBNODE &&
         index_entry_.size >= offsetof(Data::IndexEntry, stream) + 8;
}

ULONGLONG IndexEntry::GetSubNodeVCN() const noexcept
{
  return *reinterpret_cast<const ULONGLONG*>(
      reinterpret_cast<const BYTE*>(&index_entry_) + index_entry_.size - 8);
}

}  // namespace NtfsBrowser
