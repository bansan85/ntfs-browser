#include "attr/list.h"

#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>  // IWYU pragma: keep
#include <ntfs-browser/ntfs-volume.h>     // IWYU pragma: keep

#include "attr/non-resident.h"
#include "attr/resident.h"
#include "attr/slot.h"
#include "data/attribute-list.h"
#include "io/file-record-impl.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"

namespace NtfsBrowser::Attr {

namespace {

// Width of the Attr::Type field in a chain key. Every Attr::Type fits 16 bits.
constexpr unsigned chain_key_type_bits = 16;

// Selects the Attr::Type bits of a chain key.
constexpr ULONGLONG chain_key_type_mask = 0xFFFFU;

// Packs (record_ref, attr_type) into one key. record_ref fits the high 48
// bits (MftSegmentReference::segment_number is 48-bit); every Attr::Type
// fits the low 16 bits.
ULONGLONG MakeChainKey(ULONGLONG record_ref, Type attr_type) noexcept {
  return (record_ref << chain_key_type_bits) |
         (static_cast<ULONGLONG>(attr_type) & chain_key_type_mask);
}

}  // namespace

template <typename Resident, Cache::Strategy S>
AttrList<Resident, S>::AttrList(const HeaderCommon& ahc,
                                Io::FileRecord<S>& file_record,
                                std::unordered_set<ULONGLONG>& attr_list_chain)
    : Resident(ahc, file_record) {
  Log::Trace("Attribute: Attribute List");
  if (!file_record.impl_->file_reference) {
    throw std::runtime_error("Missing file reference\n");
  }

  const bool recover = file_record.GetVolume().GetOptions().recover_errors;
  ULONGLONG offset = 0;
  std::optional<ULONGLONG> len = 0;
  Data::AttributeList al_record{};
  bool truncated = false;

  // Marks this record's own chain key first, so a cycle back to it is caught.
  attr_list_chain.insert(
      MakeChainKey(*file_record.impl_->file_reference, Type::AttributeList));

  while ((
      len = this->ReadData(offset, {reinterpret_cast<BYTE*>(&al_record),
                                    Data::attribute_list_entry_header_size}))) {
    if (*len != Data::attribute_list_entry_header_size) {
      // A resident list's normal end already exited the loop above (ReadData
      // returns nullopt at offset >= size); a non-resident list's normal end
      // is this exact zero-byte read, landing precisely on the declared
      // size. Anything else here is a truncated entry.
      if (*len != 0 || offset != this->GetDataSize()) {
        truncated = true;
        Log::Recoverable(
            recover,
            "Attribute List: ReadData returned {} bytes, "
            "expected {} - stopping",
            *len,
            static_cast<ULONGLONG>(Data::attribute_list_entry_header_size));
      }
      break;
    }

    if (!Attr::IsValidAttrType(al_record.attr_type)) {
      throw std::runtime_error(
          "Attribute List parse error (al_record.attr_type).\n");
    }

    Log::Debug("Attribute List: 0x{:04x}",
               static_cast<DWORD>(al_record.attr_type));

    ResolveEntry(al_record, file_record, attr_list_chain, recover);

    if (al_record.record_size != 0 &&
        al_record.record_size < Data::attribute_list_entry_header_size) {
      truncated = true;
      Log::Recoverable(
          recover,
          "Attribute List: record_size {} is smaller than the "
          "entry header {} - stopping",
          al_record.record_size,
          static_cast<WORD>(Data::attribute_list_entry_header_size));
      break;
    }
    if (al_record.record_size == 0) {
      throw std::runtime_error(
          "Attribute List with zero record size has endless loop.\n");
    }
    offset += al_record.record_size;
  }

  // A resident list's normal end (ReadData returning nullopt) can still land
  // past its own declared size, when the last entry's record_size
  // overshoots it - not caught by either check above.
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

// Moves the attributes entry names, from the extension record that holds them,
// into file_record. A contained or unwanted attribute is skipped.
template <typename Resident, Cache::Strategy S>
void AttrList<Resident, S>::ResolveEntry(
    const Data::AttributeList& entry, Io::FileRecord<S>& file_record,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover) {
  const ULONGLONG record_ref = entry.base_ref.segment_number;
  const Mask attr_mask = Attr::AttrMask(entry.attr_type);
  if (!file_record.impl_->file_reference) {
    throw std::runtime_error("Missing file reference\n");
  }
  const ULONGLONG self_ref = *file_record.impl_->file_reference;
  // Skip contained attributes
  // Skip unwanted attributes
  if (record_ref == self_ref ||
      !static_cast<bool>(attr_mask & file_record.impl_->attr_mask)) {
    return;
  }

  if (!attr_list_chain.insert(MakeChainKey(record_ref, entry.attr_type))
           .second) {
    Log::Warn(
        "Attribute List: record {}, type 0x{:04x} already resolved in "
        "this chain, skipping",
        record_ref, static_cast<DWORD>(entry.attr_type));
    return;
  }

  // Owned by file_record, not by this object: the attributes moved into
  // file_record below keep pointing into frnew's bytes.
  file_record.impl_->extension_records.emplace_back(file_record.GetVolume());
  Io::FileRecord<S>& frnew = file_record.impl_->extension_records.back();

  frnew.impl_->attr_mask = attr_mask;
  frnew.impl_->attr_raw_call_back = file_record.impl_->attr_raw_call_back;
  if (!frnew.ParseFileRecord(record_ref)) {
    throw std::runtime_error("Attribute List parse error (ParseFileRecord).\n");
  }

  // A record another file reused since the list was written is not
  // this file's extension: its attributes belong to someone else.
  const bool genuine = Mft::IsGenuineExtensionRecord(
      entry.base_ref.sequence_number, frnew.GetSequenceNumber(),
      frnew.GetBaseRecordReference(), self_ref & Mft::mft_record_number_mask);
  if (!genuine) {
    file_record.impl_->extension_records.pop_back();
    Log::Recoverable(recover,
                     "Attribute List: record {} is not an extension of "
                     "record {} (reused or foreign) - skipping",
                     record_ref, self_ref);
    if (!recover) {
      throw std::runtime_error(
          "Attribute List names a record of another file.\n");
    }
    return;
  }

  if (!frnew.impl_->ParseAttrs(attr_list_chain)) {
    throw std::runtime_error("Attribute List parse error (ParseAttrs).\n");
  }

  // Insert new found AttrList to fr.AttrList
  std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      frnew.GetAttr(entry.attr_type);
  for (std::unique_ptr<Attr::AttrBase<S>>& veci : vec) {
    file_record.impl_->attr_list.at(Attr::AttrIndex(entry.attr_type))
        .push_back(std::move(veci));
  }
  vec.clear();
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

}  // namespace NtfsBrowser::Attr
