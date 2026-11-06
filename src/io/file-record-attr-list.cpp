#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/list-chain.h"
#include "attr/run-list.h"
#include "attr/slot.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "io/file-record-impl.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Offset in record of its first attribute header of type attr_type, going by
// the headers alone. Empty when there is none, or when a header before it is
// broken.
std::optional<DWORD> FindAttrHeader(const Data::FileRecordHeader& record,
                                    DWORD record_size,
                                    Attr::Type attr_type) noexcept {
  DWORD data_ptr = record.offset_of_attr;
  // Each header is read through a copy: its position comes from the disk.
  while (static_cast<ULONGLONG>(data_ptr) + sizeof(Attr::HeaderCommon) <=
         record_size) {
    Attr::HeaderCommon head{};
    std::memcpy(&head, &record.raw[data_ptr], sizeof(head));
    if (head.type == Attr::Type::All || head.total_size == 0 ||
        static_cast<ULONGLONG>(data_ptr) + head.total_size > record_size) {
      return std::nullopt;
    }
    if (head.type == attr_type) {
      return data_ptr;
    }
    data_ptr += head.total_size;
  }
  return std::nullopt;
}

// The non-resident header of the attribute at offset at of the record. Empty
// unless it is a whole, uncompressed list that maps its bytes from VCN 0.
std::optional<Data::HeaderNonResident>
    ReadListHeader(const Data::FileRecordHeader& record, DWORD at,
                   DWORD record_size) {
  if (record_size - at < sizeof(Data::HeaderNonResident)) {
    return std::nullopt;
  }
  Data::HeaderNonResident header{};
  std::memcpy(&header, &record.raw[at], sizeof(header));
  if (header.header.non_resident == 0 ||
      header.header.total_size < sizeof(header) ||
      Data::HasCompressedSizeField(header) || header.start_vcn != 0 ||
      header.data_run_offset >= header.header.total_size) {
    return std::nullopt;
  }
  return header;
}

}  // namespace

template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::NamesOtherBase(ULONGLONG record_number,
                                             ULONGLONG listing_record) const {
  if (!base_source) {
    return false;
  }
  const std::optional<ULONGLONG> base = base_source(record_number);
  return base && *base != listing_record;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::SkipForeignExtension(ULONGLONG record_number,
                                                   bool recover) {
  const ULONGLONG self_ref = file_reference.value_or(0);
  if (self->IsDeleted()) {
    // Freeing the file freed its extensions too, so a reused slot is normal.
    // The list names the same record once per attribute, so log it once.
    if (std::ranges::find(reused_extensions, record_number) ==
        reused_extensions.end()) {
      reused_extensions.push_back(record_number);
      Log::Debug(
          "Attribute List: record {} was reused by another file since record "
          "{} was freed - skipping",
          record_number, self_ref);
    }
    return;
  }
  Log::Recoverable(recover,
                   "Attribute List: record {} is not an extension of "
                   "record {} (reused or foreign) - skipping",
                   record_number, self_ref);
  if (!recover) {
    throw std::runtime_error(
        "Attribute List names a record of another file.\n");
  }
}

// Decides whether the extension record named by an $ATTRIBUTE_LIST entry is to
// be opened at all, and sets held to its bytes when the caller already has
// them. False for a record this one holds, an unwanted type, an entry already
// resolved in this chain, a slot of another base and a refused read.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::WantsExtension(
    const ExtensionRequest& request,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover,
    std::span<const BYTE>& held) {
  const auto [record_number, attr_type, claim_only] = request;
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  // Set by ParseRecord(), which every attribute parse follows.
  const ULONGLONG self_ref = file_reference.value_or(0);
  // Skip contained attributes
  // Skip unwanted attributes
  if (record_number == self_ref ||
      (!static_cast<bool>(entry_mask & attr_mask) && !claim_only)) {
    return false;
  }

  if (!attr_list_chain.insert(Attr::MakeChainKey(record_number, attr_type))
           .second) {
    Log::Info(
        "Attribute List: record {}, type 0x{:04x} already resolved in "
        "this chain, skipping",
        record_number, static_cast<DWORD>(attr_type));
    return false;
  }

  // An empty span makes ParseFileRecord() read the record from the disk,
  // unless the caller already saw that the slot names another base.
  held = extension_source ? extension_source(record_number)
                          : std::span<const BYTE>{};
  if (held.empty() &&
      NamesOtherBase(record_number, self_ref & Mft::mft_record_number_mask)) {
    if (!claim_only) {
      SkipForeignExtension(record_number, recover);
    }
    return false;
  }
  return !(held.empty() && self->RefuseRead());
}

// Opens extension record record_number, owned by this record, with the masks
// and sources of this one. The attributes moved into this record keep pointing
// into its bytes. Null when a claim-only record cannot be parsed; throws when
// a wanted one cannot.
template <Cache::Strategy S>
Io::FileRecord<S>*
    Io::FileRecord<S>::Impl::OpenExtension(const ExtensionRequest& request,
                                           std::span<const BYTE> held) {
  const auto [record_number, attr_type, claim_only] = request;
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  extension_records.emplace_back(*volume);
  Io::FileRecord<S>& frnew = extension_records.back();

  frnew.impl_->attr_mask = claim_only ? static_cast<Attr::Mask>(0) : entry_mask;
  frnew.impl_->claim_mask =
      claim_only ? entry_mask : static_cast<Attr::Mask>(0);
  frnew.impl_->attr_raw_call_back = attr_raw_call_back;
  frnew.impl_->extension_source = extension_source;
  frnew.impl_->base_source = base_source;
  frnew.impl_->read_refusal = read_refusal;
  if (!frnew.ParseFileRecord(record_number, held)) {
    if (claim_only) {
      extension_records.pop_back();
      return nullptr;
    }
    throw std::runtime_error("Attribute List parse error (ParseFileRecord).\n");
  }
  return &frnew;
}

// A record another file reused since the list was written is not this file's
// extension: its attributes belong to someone else.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::IsGenuineExtension(
    const Io::FileRecord<S>& extension, WORD sequence_number) const {
  const ULONGLONG listing_record =
      file_reference.value_or(0) & Mft::mft_record_number_mask;
  return self->IsDeleted()
             ? Mft::IsGenuineFreedExtensionRecord(
                   sequence_number, extension.GetSequenceNumber(),
                   !extension.IsDeleted(), extension.GetBaseRecordReference(),
                   listing_record)
             : Mft::IsGenuineExtensionRecord(
                   sequence_number, extension.GetSequenceNumber(),
                   extension.GetBaseRecordReference(), listing_record);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover) {
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  // A type that is only claimed is listed, not parsed: nothing about it may
  // fail the parse of this record.
  const bool claim_only = !static_cast<bool>(entry_mask & attr_mask) &&
                          static_cast<bool>(entry_mask & claim_mask);
  std::span<const BYTE> held;
  const ExtensionRequest request{record_number, attr_type, claim_only};
  if (!WantsExtension(request, attr_list_chain, recover, held)) {
    return;
  }

  Io::FileRecord<S>* const opened = OpenExtension(request, held);
  if (opened == nullptr) {
    return;
  }
  Io::FileRecord<S>& frnew = *opened;

  if (!IsGenuineExtension(frnew, sequence_number)) {
    extension_records.pop_back();
    if (!claim_only) {
      SkipForeignExtension(record_number, recover);
    }
    return;
  }

  if (!frnew.impl_->ParseAttrs(attr_list_chain) && !claim_only) {
    throw std::runtime_error("Attribute List parse error (ParseAttrs).\n");
  }
  // An extension of a freed record is freed too, and its own list may be gone.
  if (frnew.impl_->overwritten_list && !claim_only) {
    self->NoteOverwrittenAttrList();
  }

  if (claim_only) {
    claimed_layouts.insert(claimed_layouts.end(),
                           frnew.impl_->claimed_layouts.begin(),
                           frnew.impl_->claimed_layouts.end());
    return;
  }
  // Insert new found AttrList to fr.AttrList
  std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      frnew.GetAttr(attr_type);
  for (std::unique_ptr<Attr::AttrBase<S>>& veci : vec) {
    attr_list.at(Attr::AttrIndex(attr_type)).push_back(std::move(veci));
  }
  vec.clear();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::HasAttrHeader(
    Attr::Type attr_type) const noexcept {
  return file_record && FindAttrHeader(*file_record->GetData(),
                                       volume->GetFileRecordSize(), attr_type)
                            .has_value();
}

template <Cache::Strategy S>
std::vector<Alloc::Extent>
    Io::FileRecord<S>::Impl::AttributeListExtents(ULONGLONG max_bytes) const {
  const ULONGLONG cluster_size = volume->GetClusterSize();
  if (!file_record || max_bytes == 0 || cluster_size == 0) {
    return {};
  }
  const Data::FileRecordHeader& data = *file_record->GetData();
  const std::optional<DWORD> at = FindAttrHeader(
      data, volume->GetFileRecordSize(), Attr::Type::AttributeList);
  if (!at) {
    return {};
  }
  const std::optional<Data::HeaderNonResident> header =
      ReadListHeader(data, *at, volume->GetFileRecordSize());
  if (!header) {
    return {};
  }

  // FindAttrHeader() checked that the attribute lies in the record.
  const std::span<const BYTE> run_bytes =
      std::span<const BYTE>{&data.raw[*at], header->header.total_size}.subspan(
          header->data_run_offset);
  Attr::RunList runs(Data::SpannedClusters(*header));
  // Recovering: a damaged run list gives the runs before the damage, and is
  // logged as recovered, not as an error. ParseAttrs() judges it.
  runs.Parse(run_bytes, Data::VcnSpan(*header), true);

  const ULONGLONG size = (std::min)(header->real_size, max_bytes);
  const ULONGLONG end_vcn =
      size / cluster_size + (size % cluster_size != 0 ? 1 : 0);
  // The byte address of a cluster MUST fit a LONGLONG, as in ReadClusters().
  const ULONGLONG max_lcn =
      static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()) /
      cluster_size;
  return runs.ExtentsBetween(0, end_vcn, max_lcn);
}

template bool FileRecord<Cache::Strategy::NoCache>::Impl::NamesOtherBase(
    ULONGLONG record_number, ULONGLONG listing_record) const;
template bool FileRecord<Cache::Strategy::FullCache>::Impl::NamesOtherBase(
    ULONGLONG record_number, ULONGLONG listing_record) const;
template void FileRecord<Cache::Strategy::NoCache>::Impl::SkipForeignExtension(
    ULONGLONG record_number, bool recover);
template void
    FileRecord<Cache::Strategy::FullCache>::Impl::SkipForeignExtension(
        ULONGLONG record_number, bool recover);
template void FileRecord<Cache::Strategy::NoCache>::Impl::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover);
template void FileRecord<Cache::Strategy::FullCache>::Impl::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover);

template bool FileRecord<Cache::Strategy::NoCache>::Impl::HasAttrHeader(
    Attr::Type attr_type) const noexcept;
template bool FileRecord<Cache::Strategy::FullCache>::Impl::HasAttrHeader(
    Attr::Type attr_type) const noexcept;
template std::vector<Alloc::Extent>
    FileRecord<Cache::Strategy::NoCache>::Impl::AttributeListExtents(
        ULONGLONG max_bytes) const;
template std::vector<Alloc::Extent>
    FileRecord<Cache::Strategy::FullCache>::Impl::AttributeListExtents(
        ULONGLONG max_bytes) const;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
