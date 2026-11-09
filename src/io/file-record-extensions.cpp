#include "io/file-record-extensions.h"

#include <algorithm>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "attr/list-chain.h"
#include "attr/mask.h"
#include "attr/slot.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record-impl.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

template <Cache::Strategy S>
Io::FileRecord<S>::Impl::ExtensionImporter::ExtensionImporter(
    Impl& impl, std::unordered_set<ULONGLONG>& attr_list_chain,
    bool recover) noexcept
    : impl_(impl), attr_list_chain_(attr_list_chain), recover_(recover) {}

template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ExtensionImporter::NamesOtherBase(
    ULONGLONG record_number, ULONGLONG listing_record) const {
  if (!impl_.base_source) {
    return false;
  }
  const std::optional<ULONGLONG> base = impl_.base_source(record_number);
  return base && *base != listing_record;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ExtensionImporter::SkipForeign(
    ULONGLONG record_number) {
  const ULONGLONG self_ref = impl_.file_reference.value_or(0);
  if (impl_.self->IsDeleted()) {
    // Freeing the file freed its extensions too, so a reused slot is normal.
    // The list names the same record once per attribute, so log it once.
    if (std::ranges::find(impl_.reused_extensions, record_number) ==
        impl_.reused_extensions.end()) {
      impl_.reused_extensions.push_back(record_number);
      Log::Debug(
          "Attribute List: record {} was reused by another file since record "
          "{} was freed - skipping",
          record_number, self_ref);
    }
    return;
  }
  Log::Recoverable(recover_,
                   "Attribute List: record {} is not an extension of "
                   "record {} (reused or foreign) - skipping",
                   record_number, self_ref);
  if (!recover_) {
    throw std::runtime_error(
        "Attribute List names a record of another file.\n");
  }
}

// False for a record this one holds, an unwanted type, an entry already
// resolved in this chain, a slot of another base and a refused read.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ExtensionImporter::Wants(
    const Request& request, std::span<const BYTE>& held) {
  const auto [record_number, attr_type, claim_only] = request;
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  // Set by ParseRecord(), which every attribute parse follows.
  const ULONGLONG self_ref = impl_.file_reference.value_or(0);
  // Skip contained attributes
  // Skip unwanted attributes
  if (record_number == self_ref ||
      (!static_cast<bool>(entry_mask & impl_.attr_mask) && !claim_only)) {
    return false;
  }

  if (!attr_list_chain_.insert(Attr::MakeChainKey(record_number, attr_type))
           .second) {
    Log::Info(
        "Attribute List: record {}, type 0x{:04x} already resolved in "
        "this chain, skipping",
        record_number, static_cast<DWORD>(attr_type));
    return false;
  }

  // An empty span makes ParseFileRecord() read the record from the disk,
  // unless the caller already saw that the slot names another base.
  held = impl_.extension_source ? impl_.extension_source(record_number)
                                : std::span<const BYTE>{};
  if (held.empty() &&
      NamesOtherBase(record_number, self_ref & Mft::mft_record_number_mask)) {
    if (!claim_only) {
      SkipForeign(record_number);
    }
    return false;
  }
  return !(held.empty() && impl_.self->RefuseRead());
}

// The extension is owned by the record, with the masks and sources of it. The
// attributes moved into the record keep pointing into the extension's bytes.
// Throws when a wanted record cannot be parsed.
template <Cache::Strategy S>
Io::FileRecord<S>* Io::FileRecord<S>::Impl::ExtensionImporter::Open(
    const Request& request, std::span<const BYTE> held) {
  const auto [record_number, attr_type, claim_only] = request;
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  impl_.extension_records.emplace_back(*impl_.volume);
  Io::FileRecord<S>& frnew = impl_.extension_records.back();

  frnew.impl_->attr_mask = claim_only ? static_cast<Attr::Mask>(0) : entry_mask;
  frnew.impl_->claim_mask =
      claim_only ? entry_mask : static_cast<Attr::Mask>(0);
  frnew.impl_->attr_raw_call_back = impl_.attr_raw_call_back;
  frnew.impl_->extension_source = impl_.extension_source;
  frnew.impl_->base_source = impl_.base_source;
  frnew.impl_->read_refusal = impl_.read_refusal;
  if (!frnew.ParseFileRecord(record_number, held)) {
    if (claim_only) {
      impl_.extension_records.pop_back();
      return nullptr;
    }
    throw std::runtime_error("Attribute List parse error (ParseFileRecord).\n");
  }
  return &frnew;
}

// A record another file reused since the list was written is not this file's
// extension: its attributes belong to someone else.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ExtensionImporter::IsGenuine(
    const Io::FileRecord<S>& extension, WORD sequence_number) const {
  const ULONGLONG listing_record =
      impl_.file_reference.value_or(0) & Mft::mft_record_number_mask;
  return impl_.self->IsDeleted()
             ? Mft::IsGenuineFreedExtensionRecord(
                   sequence_number, extension.GetSequenceNumber(),
                   !extension.IsDeleted(), extension.GetBaseRecordReference(),
                   listing_record)
             : Mft::IsGenuineExtensionRecord(
                   sequence_number, extension.GetSequenceNumber(),
                   extension.GetBaseRecordReference(), listing_record);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ExtensionImporter::Import(ULONGLONG record_number,
                                                        WORD sequence_number,
                                                        Attr::Type attr_type) {
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  // A type that is only claimed is listed, not parsed: nothing about it may
  // fail the parse of this record.
  const bool claim_only = !static_cast<bool>(entry_mask & impl_.attr_mask) &&
                          static_cast<bool>(entry_mask & impl_.claim_mask);
  std::span<const BYTE> held;
  const Request request{record_number, attr_type, claim_only};
  if (!Wants(request, held)) {
    return;
  }

  Io::FileRecord<S>* const opened = Open(request, held);
  if (opened == nullptr) {
    return;
  }
  Io::FileRecord<S>& frnew = *opened;

  if (!IsGenuine(frnew, sequence_number)) {
    impl_.extension_records.pop_back();
    if (!claim_only) {
      SkipForeign(record_number);
    }
    return;
  }

  if (!frnew.impl_->ParseAttrs(attr_list_chain_) && !claim_only) {
    throw std::runtime_error("Attribute List parse error (ParseAttrs).\n");
  }
  // An extension of a freed record is freed too, and its own list may be gone.
  if (frnew.impl_->overwritten_list && !claim_only) {
    impl_.self->NoteOverwrittenAttrList();
  }

  if (claim_only) {
    impl_.claimed_layouts.insert(impl_.claimed_layouts.end(),
                                 frnew.impl_->claimed_layouts.begin(),
                                 frnew.impl_->claimed_layouts.end());
    return;
  }
  // Insert new found AttrList to fr.AttrList
  std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      frnew.GetAttr(attr_type);
  for (std::unique_ptr<Attr::AttrBase<S>>& veci : vec) {
    impl_.attr_list.at(Attr::AttrIndex(attr_type)).push_back(std::move(veci));
  }
  vec.clear();
}

template class FileRecord<Cache::Strategy::NoCache>::Impl::ExtensionImporter;
template class FileRecord<Cache::Strategy::FullCache>::Impl::ExtensionImporter;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
