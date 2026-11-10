#include "attr/index-root.h"

#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/index-entry-walker.h"
#include "attr/resident.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "data/index-root.h"
#include "index-entry.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

template <typename Resident, Cache::Strategy S>
AttrIndexRoot<Resident, S>::AttrIndexRoot(const HeaderCommon& ahc,
                                          const Io::FileRecord<S>& file_record)
    : Resident(ahc, file_record),
      index_root_(reinterpret_cast<const Data::IndexRoot*>(this->GetData())) {
  if (this->GetDataSize() < sizeof(Data::IndexRoot)) {
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

template <typename Resident, Cache::Strategy S>
AttrIndexRoot<Resident, S>::~AttrIndexRoot() {
  Log::Trace("AttrIndexRoot deleted");
}

// Parses every index entry, bounding each step against the resident
// attribute's own size. The entries are views into index_data_, a copy
// independent of the record's buffer. An IndexEntry made from one owns its
// bytes, independent of this object's lifetime.
template <typename Resident, Cache::Strategy S>
bool AttrIndexRoot<Resident, S>::ParseIndexEntries() {
  const bool recover = this->volume_.GetOptions().recover_errors;
  const ULONGLONG data_size = this->GetDataSize();
  index_data_.resize(data_size);
  std::memcpy(index_data_.data(), this->GetData(), data_size);
  Log::Debug("Index Root: allocated independent copy of resident data");

  const std::span<const BYTE> data(index_data_.data(), data_size);
  const auto* const index_root_copy =
      reinterpret_cast<const Data::IndexRoot*>(index_data_.data());
  constexpr size_t entry_offset_pos = offsetof(Data::IndexRoot, entry_offset);

  if (data.size() < entry_offset_pos ||
      index_root_copy->entry_offset > data.size() - entry_offset_pos) {
    Log::Recoverable(recover,
                     "Index Root: entry_offset exceeds attribute bounds");
    return recover;
  }

  // An entry's position comes from the disk, so it need not be aligned.
  IndexEntryWalker walker(IndexEntryWalker::root_messages, recover, *this,
                          realigned_);
  return walker
      .Walk(
          data.subspan(entry_offset_pos).subspan(index_root_copy->entry_offset),
          index_root_copy->total_entry_size)
      .accepted;
}

// Check if this IndexRoot contains Filename or IndexView
template <typename Resident, Cache::Strategy S>
bool AttrIndexRoot<Resident, S>::IsFileName() const noexcept {
  return index_root_->attr_type == Type::FileName;
}

template class AttrIndexRoot<AttrResidentFullCache, Cache::Strategy::FullCache>;
template class AttrIndexRoot<AttrResidentNoCache, Cache::Strategy::NoCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
