#include "attr-index-root.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <gsl/narrow>

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

template <typename RESIDENT, Strategy S>
AttrIndexRoot<RESIDENT, S>::AttrIndexRoot(const AttrHeaderCommon& ahc,
                                          const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
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
  const auto data_copy = std::make_shared<BYTE[]>(data_size);
  std::memcpy(data_copy.get(), this->GetData(), data_size);
  LogDebug("Index Root: allocated independent copy of resident data");

  const BYTE* const data_end = data_copy.get() + data_size;
  const auto* const index_root_copy =
      reinterpret_cast<const Attr::IndexRoot*>(data_copy.get());
  const auto* const entry_offset_addr =
      reinterpret_cast<const BYTE*>(&(index_root_copy->entry_offset));

  if (index_root_copy->entry_offset >
      gsl::narrow<ULONGLONG>(data_end - entry_offset_addr))
  {
    LogRecoverable(recover,
                   "Index Root: entry_offset exceeds attribute bounds");
    return recover;
  }

  // An entry's position comes from the disk, so it need not be aligned.
  const BYTE* cur = entry_offset_addr + index_root_copy->entry_offset;
  DWORD ieTotal = 0;

  while (true)
  {
    if (cur + offsetof(Data::IndexEntry, stream) > data_end)
    {
      LogRecoverable(recover,
                     "Index Root: index entry header exceeds attribute bounds");
      if (!recover)
      {
        clear();
        return false;
      }
      break;
    }
    const Data::IndexEntry head = ReadIndexEntryHeader(cur);
    if (head.size == 0 || cur + head.size > data_end)
    {
      LogRecoverable(recover,
                     "Index Root: index entry exceeds attribute bounds");
      if (!recover)
      {
        clear();
        return false;
      }
      break;
    }

    ieTotal += head.size;
    if (ieTotal > index_root_copy->total_entry_size)
    {
      LogRecoverable(recover,
                     "Index Root: index entry total exceeds the attribute's "
                     "declared entry size");
      if (!recover)
      {
        clear();
        return false;
      }
      break;
    }

    const AlignedIndexEntry ie = AlignIndexEntry(data_copy, cur, head.size);
    if (const std::optional<std::string_view> defect =
            ValidateIndexEntry(*ie.entry))
    {
      LogRecoverable(recover, "{}", *defect);
      if (!recover)
      {
        clear();
        return false;
      }
    }

    emplace_back(ie.owner, *ie.entry);

    if ((head.flags & Flag::IndexEntry::LAST) == Flag::IndexEntry::LAST)
    {
      LogTrace("Last Index Entry");
      break;
    }

    cur += head.size;  // Pick next
  }

  return true;
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
