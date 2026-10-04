#include "attr-index-root.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "attr/index-root.h"
#include "data/index-entry.h"
#include "flag/index-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

namespace
{

// Reports a defect in an index root's entries. Returns true when the attribute
// must be rejected whole: the entries parsed so far are then discarded too.
bool RejectRootOnDefect(bool recover, std::string_view defect,
                        std::vector<IndexEntry>& entries)
{
  LogRecoverable(recover, "{}", defect);
  if (recover)
  {
    return false;
  }
  entries.clear();
  return true;
}

}  // namespace

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::AttrIndexRoot(const AttrHeaderCommon& ahc,
                                          const FileRecord<S>& file_record)
    : RESIDENT(ahc, file_record),
      index_root_(reinterpret_cast<const Attr::IndexRoot*>(this->GetData()))
{
  if (this->GetDataSize() < sizeof(Attr::IndexRoot))
  {
    throw std::runtime_error("Index Root attribute smaller than expected.\n");
  }

  LogTrace("Attribute: Index Root");

  if (!IsFileName())
  {
    LogWarn("Index View not supported");
    return;
  }

  if (!ParseIndexEntries())
  {
    throw std::runtime_error(
        "Index Root attribute has a malformed index entry.\n");
  }
}

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::~AttrIndexRoot()
{
  LogTrace("AttrIndexRoot deleted");
}

// Parses every index entry, bounding each step against the resident
// attribute's own size. Every returned IndexEntry keeps its own copy of
// the backing bytes alive, independent of this object's lifetime.
template <typename RESIDENT, Strategy S>
bool AttrIndexRoot<RESIDENT, S>::ParseIndexEntries()
{
  const bool recover = this->volume_.GetOptions().recover_errors;
  const ULONGLONG data_size = this->GetDataSize();
  const auto data_copy = std::make_shared<std::vector<BYTE>>(data_size);
  std::memcpy(data_copy->data(), this->GetData(), data_size);
  LogDebug("Index Root: allocated independent copy of resident data");

  const std::span<const BYTE> data(data_copy->data(), data_size);
  const auto* const index_root_copy =
      reinterpret_cast<const Attr::IndexRoot*>(data_copy->data());
  constexpr size_t kEntryOffsetPos = offsetof(Attr::IndexRoot, entry_offset);

  if (data.size() < kEntryOffsetPos ||
      index_root_copy->entry_offset > data.size() - kEntryOffsetPos)
  {
    LogRecoverable(recover,
                   "Index Root: entry_offset exceeds attribute bounds");
    return recover;
  }

  // An entry's position comes from the disk, so it need not be aligned.
  std::span<const BYTE> cur =
      data.subspan(kEntryOffsetPos).subspan(index_root_copy->entry_offset);
  DWORD ieTotal = 0;

  while (true)
  {
    const size_t remaining = cur.size();
    if (remaining < offsetof(Data::IndexEntry, stream))
    {
      return !RejectRootOnDefect(
          recover, "Index Root: index entry header exceeds attribute bounds",
          *this);
    }
    const Data::IndexEntry head = ReadIndexEntryHeader(cur);
    if (head.size == 0 || head.size > remaining)
    {
      return !RejectRootOnDefect(
          recover, "Index Root: index entry exceeds attribute bounds", *this);
    }

    ieTotal += head.size;
    if (ieTotal > index_root_copy->total_entry_size)
    {
      return !RejectRootOnDefect(
          recover,
          "Index Root: index entry total exceeds the attribute's declared "
          "entry size",
          *this);
    }

    const AlignedIndexEntry aligned_index_entry =
        AlignIndexEntry(data_copy, cur, head.size);
    if (const std::optional<std::string_view> defect =
            ValidateIndexEntry(*aligned_index_entry.entry);
        defect && RejectRootOnDefect(recover, *defect, *this))
    {
      return false;
    }

    emplace_back(aligned_index_entry.owner, *aligned_index_entry.entry);

    if ((head.flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      LogTrace("Last Index Entry");
      return true;
    }

    cur = cur.subspan(head.size);  // Pick next
  }
}

// Check if this IndexRoot contains Filename or IndexView
template <typename RESIDENT, Strategy S>
bool AttrIndexRoot<RESIDENT, S>::IsFileName() const noexcept
{
  return index_root_->attr_type == AttrType::FILE_NAME;
}

template class AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
