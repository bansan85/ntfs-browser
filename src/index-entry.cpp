#include "data/index-entry.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <ntfs-browser/index-entry.h>

#include "attr/filename.h"
#include "flag/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

Data::IndexEntry ReadIndexEntryHeader(std::span<const BYTE> bytes) noexcept
{
  Data::IndexEntry header{};
  std::memcpy(&header, bytes.data(), offsetof(Data::IndexEntry, stream));
  return header;
}

const Data::IndexEntry&
    AlignIndexEntry(std::vector<std::vector<BYTE>>& realigned,
                    std::span<const BYTE> bytes, size_t size)
{
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) %
          alignof(Data::IndexEntry) ==
      0)
  {
    return *reinterpret_cast<const Data::IndexEntry*>(bytes.data());
  }

  // The fixed part is read even from an entry whose size is smaller than it.
  const size_t copied = std::max(size, offsetof(Data::IndexEntry, stream));
  auto& copy =
      realigned.emplace_back(std::max(copied, sizeof(Data::IndexEntry)));
  std::memcpy(copy.data(), bytes.data(), copied);
  return *reinterpret_cast<const Data::IndexEntry*>(copy.data());
}

std::optional<std::string_view>
    ValidateIndexEntry(const Data::IndexEntry& index_entry) noexcept
{
  if (index_entry.stream_size != 0)
  {
    // stream_size has no guaranteed relation to ie.size; derive room for the
    // stream from ie.size instead.
    const size_t stream_offset = offsetof(Data::IndexEntry, stream);
    if (index_entry.size <= stream_offset)
    {
      return "Index Entry stream exceeds entry bounds";
    }
    const size_t available = index_entry.size - stream_offset;
    if (available < offsetof(Attr::Filename, name))
    {
      return "Index Entry stream smaller than expected";
    }

    const auto& filename =
        *reinterpret_cast<const Attr::Filename*>(&index_entry.stream);
    if (available <
        offsetof(Attr::Filename, name) +
            (static_cast<size_t>(filename.name_length) * sizeof(WORD)))
    {
      return "Index Entry Filename name exceeds entry bounds";
    }
  }

  // GetSubNodeVCN() reads 8 bytes at size - 8, unchecked: a SUBNODE entry
  // must have room for that field regardless of whether it also has a name.
  if ((index_entry.flags & Flag::IndexEntry::SUBNODE) ==
          Flag::IndexEntry::SUBNODE &&
      index_entry.size < offsetof(Data::IndexEntry, stream) + sizeof(ULONGLONG))
  {
    return "Index Entry is a sub-node pointer too small for its VCN field";
  }

  return std::nullopt;
}

IndexEntryView::IndexEntryView(const Data::IndexEntry& index_entry)
    : index_entry_(&index_entry)
{
  LogTrace("Index Entry");

  if (IsSubNodePtr())
  {
    LogTrace("Points to sub-node");
  }

  if (index_entry.stream_size == 0)
  {
    LogInfo("No Filename stream found");
    return;
  }

  // The caller (AttrIndexAlloc/AttrIndexRoot) already validated and logged
  // this same defect, at the level it can act on (strict vs. recovering).
  if (ValidateIndexEntry(index_entry))
  {
    return;
  }

  SetFilename(*reinterpret_cast<const Attr::Filename*>(&index_entry.stream));
}

void IndexEntryView::Rebind(const Data::IndexEntry& index_entry)
{
  index_entry_ = &index_entry;

  // The copy holds the same bytes, so the constructor above accepted or
  // rejected the name exactly as it did for the original.
  if (index_entry.stream_size != 0 && !ValidateIndexEntry(index_entry))
  {
    CopyFilename(*this,
                 *reinterpret_cast<const Attr::Filename*>(&index_entry.stream));
  }
}

namespace
{

// Copies the entry's own bytes, whatever block they sit in. The buffer is
// padded to a whole Data::IndexEntry, since its fixed part is read even from
// an entry whose size is smaller than that.
std::vector<BYTE> CopyEntryBytes(const Data::IndexEntry& index_entry)
{
  const size_t copied =
      std::max<size_t>(index_entry.size, offsetof(Data::IndexEntry, stream));
  std::vector<BYTE> bytes(std::max(copied, sizeof(Data::IndexEntry)));
  std::memcpy(bytes.data(), &index_entry, copied);
  return bytes;
}

}  // namespace

IndexEntry::IndexEntry(const IndexEntryView& view)
    : IndexEntryView(view), bytes_(CopyEntryBytes(*view.index_entry_))
{
  Rebind(*reinterpret_cast<const Data::IndexEntry*>(bytes_.data()));
}

IndexEntry::IndexEntry(IndexEntry const& other)
    : IndexEntryView(other), bytes_(other.bytes_)
{
  Rebind(*reinterpret_cast<const Data::IndexEntry*>(bytes_.data()));
}

ULONGLONG IndexEntryView::GetFileReference() const noexcept
{
  return index_entry_->mft_index;
}

WORD IndexEntryView::GetSequenceNumber() const noexcept
{
  return static_cast<WORD>(index_entry_->mft_sn);
}

bool IndexEntryView::IsSubNodePtr() const noexcept
{
  // A recovering parse still keeps a too-small SUBNODE entry (matching the
  // matrix's "kept nameless" disposition), but must never let it be treated
  // as a usable sub-node pointer: GetSubNodeVCN() reads unchecked at size-8.
  return (index_entry_->flags & Flag::IndexEntry::SUBNODE) ==
             Flag::IndexEntry::SUBNODE &&
         index_entry_->size >=
             offsetof(Data::IndexEntry, stream) + sizeof(ULONGLONG);
}

ULONGLONG IndexEntryView::GetSubNodeVCN() const noexcept
{
  // size - 8 need not be aligned: the size is not checked for it.
  ULONGLONG vcn = 0;
  const std::span<const BYTE> raw(reinterpret_cast<const BYTE*>(index_entry_),
                                  index_entry_->size);
  // HasSubNode() guarantees raw.size() >= sizeof(vcn).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  std::memcpy(&vcn, &raw[raw.size() - sizeof(vcn)], sizeof(vcn));
  return vcn;
}

}  // namespace NtfsBrowser
