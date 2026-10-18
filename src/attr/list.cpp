#include "attr/list.h"

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>  // IWYU pragma: keep
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/list-chain.h"
#include "attr/non-resident.h"
#include "attr/resident.h"
#include "attr/slot.h"
#include "data/attribute-list.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

namespace {

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

  const bool recover = file_record.GetVolume().GetOptions().recover_errors;
  ULONGLONG offset = 0;
  Data::AttributeList al_record{};
  bool truncated = false;

  // Marks this record's own chain key first, so a cycle back to it is caught.
  attr_list_chain.insert(
      MakeChainKey(*file_record.GetFileReference(), Type::AttributeList));

  while (ReadEntryHeader(offset, al_record, recover, truncated)) {
    if (!Attr::IsValidAttrType(al_record.attr_type)) {
      throw std::runtime_error(
          "Attribute List parse error (al_record.attr_type).\n");
    }

    Log::Debug("Attribute List: 0x{:04x}",
               static_cast<DWORD>(al_record.attr_type));

    file_record.ImportAttrListEntry(
        al_record.base_ref.segment_number, al_record.base_ref.sequence_number,
        al_record.attr_type, attr_list_chain, recover);

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
    Log::Recoverable(recover,
                     "Attribute List ended at offset {} instead of its "
                     "declared size {}.",
                     offset, this->GetDataSize());
  }

  if (truncated && !recover) {
    throw std::runtime_error("Attribute List is truncated.\n");
  }
}

// Reads the header of the entry at offset. Returns false when the walk ends:
// at a normal end, or at a truncated entry, which sets truncated and is logged.
template <typename Resident, Cache::Strategy S>
bool AttrList<Resident, S>::ReadEntryHeader(ULONGLONG offset,
                                            Data::AttributeList& entry,
                                            bool recover, bool& truncated) {
  const std::optional<ULONGLONG> len =
      this->ReadData(offset, {reinterpret_cast<BYTE*>(&entry),
                              Data::attribute_list_entry_header_size});
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
    Log::Recoverable(
        recover,
        "Attribute List: ReadData returned {} bytes, "
        "expected {} - stopping",
        *len, static_cast<ULONGLONG>(Data::attribute_list_entry_header_size));
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
