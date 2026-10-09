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

namespace NtfsBrowser {

struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

namespace {

// Reports a defect in an index root's entries. Returns true when the attribute
// must be rejected whole: the entries parsed so far are then discarded too.
bool RejectRootOnDefect(bool recover, std::string_view defect,
                        std::vector<IndexEntryView>& entries) {
  Log::Recoverable(recover, "{}", defect);
  if (recover) {
    return false;
  }
  entries.clear();
  return true;
}

}  // namespace

template <typename Resident, Strategy S>
AttrIndexRoot<Resident, S>::AttrIndexRoot(const AttrHeaderCommon& ahc,
                                          const FileRecord<S>& file_record)
    : Resident(ahc, file_record),
      index_root_(reinterpret_cast<const Attr::IndexRoot*>(this->GetData())) {
  if (this->GetDataSize() < sizeof(Attr::IndexRoot)) {
    throw std::runtime_error("Index Root attribute smaller than expected.\n");
  }

  Log::Trace("Attribute: Index Root");

  if (!IsFileName()) {
    Log::Warn("Index View not supported");
    return;
  }

  if (!ParseIndexEntries()) {
    throw std::runtime_error(
        "Index Root attribute has a malformed index entry.\n");
  }
}

template <typename Resident, Strategy S>
AttrIndexRoot<Resident, S>::~AttrIndexRoot() {
  Log::Trace("AttrIndexRoot deleted");
}

// Parses every index entry, bounding each step against the resident
// attribute's own size. The entries are views into index_data_, a copy
// independent of the record's buffer. An IndexEntry made from one owns its
// bytes, independent of this object's lifetime.
template <typename Resident, Strategy S>
bool AttrIndexRoot<Resident, S>::ParseIndexEntries() {
  const bool recover = this->volume_.GetOptions().recover_errors;
  const ULONGLONG data_size = this->GetDataSize();
  index_data_.resize(data_size);
  std::memcpy(index_data_.data(), this->GetData(), data_size);
  Log::Debug("Index Root: allocated independent copy of resident data");

  const std::span<const BYTE> data(index_data_.data(), data_size);
  const auto* const index_root_copy =
      reinterpret_cast<const Attr::IndexRoot*>(index_data_.data());
  constexpr size_t entry_offset_pos = offsetof(Attr::IndexRoot, entry_offset);

  if (data.size() < entry_offset_pos ||
      index_root_copy->entry_offset > data.size() - entry_offset_pos) {
    Log::Recoverable(recover,
                     "Index Root: entry_offset exceeds attribute bounds");
    return recover;
  }

  // An entry's position comes from the disk, so it need not be aligned.
  std::span<const BYTE> cur =
      data.subspan(entry_offset_pos).subspan(index_root_copy->entry_offset);
  DWORD ie_total = 0;

  while (true) {
    const size_t remaining = cur.size();
    if (remaining < offsetof(Data::IndexEntry, stream)) {
      return !RejectRootOnDefect(
          recover, "Index Root: index entry header exceeds attribute bounds",
          *this);
    }
    const Data::IndexEntry head = Data::ReadIndexEntryHeader(cur);
    if (head.size == 0 || head.size > remaining) {
      return !RejectRootOnDefect(
          recover, "Index Root: index entry exceeds attribute bounds", *this);
    }

    ie_total += head.size;
    if (ie_total > index_root_copy->total_entry_size) {
      return !RejectRootOnDefect(
          recover,
          "Index Root: index entry total exceeds the attribute's declared "
          "entry size",
          *this);
    }

    const Data::IndexEntry& aligned_index_entry =
        Data::AlignIndexEntry(realigned_, cur, head.size);
    if (const std::optional<std::string_view> defect =
            Data::ValidateIndexEntry(aligned_index_entry);
        defect && RejectRootOnDefect(recover, *defect, *this)) {
      return false;
    }

    emplace_back(aligned_index_entry);

    if ((head.flags & Flag::IndexEntry::Last) == Flag::IndexEntry::Last) {
      Log::Trace("Last Index Entry");
      return true;
    }

    cur = cur.subspan(head.size);  // Pick next
  }
}

// Check if this IndexRoot contains Filename or IndexView
template <typename Resident, Strategy S>
bool AttrIndexRoot<Resident, S>::IsFileName() const noexcept {
  return index_root_->attr_type == AttrType::FileName;
}

template class AttrIndexRoot<AttrResidentFullCache, Strategy::FullCache>;
template class AttrIndexRoot<AttrResidentNoCache, Strategy::NoCache>;

}  // namespace NtfsBrowser
