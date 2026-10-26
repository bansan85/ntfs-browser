#include "attr/list.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/list-chain.h"
#include "attr/non-resident.h"
#include "attr/resident.h"
#include "attr/slot.h"
#include "data/attribute-list.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

namespace {

// Largest $ATTRIBUTE_LIST Windows writes. A longer one is read entry by entry,
// so a forged size never sizes an allocation.
constexpr ULONGLONG max_buffered_list_bytes = ULONGLONG{256} * 1024;

// Checks an entry's record_size before the walk advances by it. Returns false,
// and logs, when it is smaller than the entry header. Zero throws: the walk
// would never advance.
bool CheckRecordSize(const Data::AttributeList& entry, bool recover) {
  if (entry.record_size == 0) {
    throw std::runtime_error(
        "Attribute List with zero record size has endless loop.\n");
  }
  if (entry.record_size < Data::attribute_list_entry_header_size) {
    Log::Recoverable(recover,
                     "Attribute List: record_size {} is smaller than the "
                     "entry header {} - stopping",
                     entry.record_size,
                     static_cast<WORD>(Data::attribute_list_entry_header_size));
    return false;
  }
  return true;
}

}  // namespace

template <typename Resident, Cache::Strategy S>
AttrList<Resident, S>::AttrList(const HeaderCommon& ahc,
                                Io::FileRecord<S>& file_record,
                                std::unordered_set<ULONGLONG>& attr_list_chain)
    : Resident(ahc, file_record) {
  Log::Trace("Attribute: Attribute List");
  // AttrParser::Parse() runs only on a record that ParseFileRecord() read,
  // and that sets file_reference too.
  assert(file_record.GetFileReference().has_value());
  const ULONGLONG self_ref = file_record.GetFileReference().value_or(0);

  const bool recover = file_record.GetVolume().GetOptions().recover_errors;
  ULONGLONG offset = 0;
  Data::AttributeList al_record{};
  bool truncated = false;
  // NTFS frees the clusters of a freed record's non-resident list, so another
  // file may have written over them: garbage there is no defect. A resident
  // list lies inside the record, which nothing else writes.
  bool overwritable = false;
  if constexpr (std::is_same_v<Resident, AttrNonResident<S>>) {
    overwritable = file_record.IsDeleted();
  }

  // Marks this record's own chain key first, so a cycle back to it is caught.
  attr_list_chain.insert(MakeChainKey(self_ref, Type::AttributeList));

  // Under NoCache, each entry read would read its clusters from the disk again.
  ListCache cache = MakeListCache();
  while (ReadEntryHeader(offset, al_record, recover, overwritable, truncated,
                         cache)) {
    if (!Attr::IsValidAttrType(al_record.attr_type)) {
      if (overwritable) {
        file_record.NoteOverwrittenAttrList();
        return;
      }
      throw std::runtime_error(
          "Attribute List parse error (al_record.attr_type).\n");
    }

    Log::Debug("Attribute List: 0x{:04x}",
               static_cast<DWORD>(al_record.attr_type));

    file_record.ImportAttrListEntry(
        al_record.base_ref.segment_number, al_record.base_ref.sequence_number,
        al_record.attr_type, attr_list_chain, recover);

    if (overwritable &&
        al_record.record_size < Data::attribute_list_entry_header_size) {
      file_record.NoteOverwrittenAttrList();
      return;
    }
    if (!CheckRecordSize(al_record, recover)) {
      truncated = true;
      break;
    }
    offset += al_record.record_size;
  }

  // A resident list's normal end (ReadData returning nullopt) can still land
  // past its own declared size, when the last entry's record_size overshoots
  // it - not caught by either check above.
  if (!truncated && offset != this->GetDataSize()) {
    truncated = true;
    if (!overwritable) {
      Log::Recoverable(recover,
                       "Attribute List ended at offset {} instead of its "
                       "declared size {}.",
                       offset, this->GetDataSize());
    }
  }

  if (truncated && overwritable) {
    file_record.NoteOverwrittenAttrList();
    return;
  }
  if (truncated && !recover) {
    throw std::runtime_error("Attribute List is truncated.\n");
  }
}

// An empty cache for a non-resident list of at most max_buffered_list_bytes.
// Else one with no bytes, for a resident list, whose bytes sit in the record.
template <typename Resident, Cache::Strategy S>
typename AttrList<Resident, S>::ListCache
    AttrList<Resident, S>::MakeListCache() const {
  ListCache cache;
  if constexpr (std::is_same_v<Resident, AttrNonResident<S>>) {
    const ULONGLONG size = this->GetDataSize();
    const ULONGLONG cluster_size = this->GetClusterSize();
    if (size != 0 && size <= max_buffered_list_bytes && cluster_size != 0) {
      cache.bytes.resize(gsl::narrow<size_t>(size));
      cache.loaded.resize(
          gsl::narrow<size_t>((size + cluster_size - 1) / cluster_size));
    }
  }
  return cache;
}

// Reads the list bytes at offset into out, as ReadData() does. A cluster is
// read the first time an entry needs it. The walk ends at the first read that
// fails, so a failed cluster is never asked for again.
template <typename Resident, Cache::Strategy S>
std::optional<ULONGLONG>
    AttrList<Resident, S>::ReadListBytes(ULONGLONG offset, std::span<BYTE> out,
                                         ListCache& cache) const {
  if (cache.bytes.empty()) {
    return this->ReadData(offset, out);
  }
  const ULONGLONG size = cache.bytes.size();
  // As AttrNonResident::ReadData(): past the end fails, the end reads nothing.
  if (offset > size) {
    return {};
  }
  const ULONGLONG len = (std::min<ULONGLONG>)(out.size(), size - offset);
  const ULONGLONG cluster_size = this->GetClusterSize();
  for (ULONGLONG cluster = offset / cluster_size;
       cluster * cluster_size < offset + len; cluster++) {
    const auto index = gsl::narrow<size_t>(cluster);
    if (cache.loaded.at(index)) {
      continue;
    }
    const ULONGLONG first = cluster * cluster_size;
    const ULONGLONG count = (std::min)(cluster_size, size - first);
    const std::optional<ULONGLONG> read = this->ReadData(
        first, std::span<BYTE>{cache.bytes}.subspan(
                   gsl::narrow<size_t>(first), gsl::narrow<size_t>(count)));
    if (!read || *read != count) {
      return {};
    }
    cache.loaded.at(index) = true;
  }
  std::copy_n(
      std::next(cache.bytes.begin(), gsl::narrow<std::ptrdiff_t>(offset)),
      gsl::narrow<size_t>(len), out.begin());
  return len;
}

// Reads the header of the entry at offset. Returns false when the walk ends:
// at a normal end, or at a truncated entry, which sets truncated and is logged,
// unless overwritable: the caller then treats it as an overwritten list.
template <typename Resident, Cache::Strategy S>
bool AttrList<Resident, S>::ReadEntryHeader(ULONGLONG offset,
                                            Data::AttributeList& entry,
                                            bool recover, bool overwritable,
                                            bool& truncated, ListCache& cache) {
  const std::optional<ULONGLONG> len = ReadListBytes(
      offset,
      {reinterpret_cast<BYTE*>(&entry), Data::attribute_list_entry_header_size},
      cache);
  if (!len) {
    // A resident list's normal end: ReadData returns nullopt at offset >= size.
    return false;
  }
  if (*len == Data::attribute_list_entry_header_size) {
    return true;
  }

  // A non-resident list's normal end is this exact zero-byte read, landing
  // precisely on the declared size. Anything else here is a truncated entry.
  if (*len != 0 || offset != this->GetDataSize()) {
    truncated = true;
    if (!overwritable) {
      Log::Recoverable(
          recover,
          "Attribute List: ReadData returned {} bytes, "
          "expected {} - stopping",
          *len, static_cast<ULONGLONG>(Data::attribute_list_entry_header_size));
    }
  }
  return false;
}

template <typename Resident, Cache::Strategy S>
AttrList<Resident, S>::~AttrList() {
  Log::Trace("AttrList deleted");
}

template class AttrList<AttrNonResident<Cache::Strategy::FullCache>,
                        Cache::Strategy::FullCache>;
template class AttrList<AttrNonResident<Cache::Strategy::NoCache>,
                        Cache::Strategy::NoCache>;
template class AttrList<AttrResidentFullCache, Cache::Strategy::FullCache>;
template class AttrList<AttrResidentNoCache, Cache::Strategy::NoCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
