#include "io/file-record.h"

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/data-state.h"
#include "alloc/types.h"
#include "attr/base.h"
#include "attr/defines.h"
#include "attr/file-name.h"
#include "attr/slot.h"
#include "attr/std-info.h"
#include "cache/strategy.h"
#include "data/file-record-flag.h"
#include "filename.h"
#include "index-entry.h"
#include "io/file-record-attr-types.h"
#include "io/file-record-extensions.h"
#include "io/file-record-impl.h"
#include "io/file-record-index.h"
#include "io/raw-attr-headers.h"
#include "io/record-loader.h"
#include "io/slack-name-carver.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "ntfs-volume.h"
#include "utf/utf.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Attr {

template <Cache::Strategy S>
class AttrBase;
template <typename Resident, Cache::Strategy S>
class AttrFileName;

}  // namespace Attr

namespace Record {

template <Cache::Strategy S>
struct HeaderImpl;

}  // namespace Record

namespace UpCase {

class Table;

}  // namespace UpCase
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The $STANDARD_INFORMATION wrapper of strategy S.
template <Cache::Strategy S>
using StdInfoType = Attr::AttrStdInfo<ResidentOf<S>, S>;

}  // namespace

template <Cache::Strategy S>
Io::FileRecord<S>::Impl::Impl(Io::FileRecord<S>& self,
                              const NtfsVolume<S>& volume) noexcept
    : self(&self), volume(&volume) {}

// Returns the record's $STANDARD_INFORMATION wrapper, or nullptr if it has
// none.
template <Cache::Strategy S>
const StdInfoType<S>* Io::FileRecord<S>::Impl::StdInfo() const noexcept {
  const Io::FileRecord<S>& record = *self;
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record.GetAttr(Attr::Type::StandardInformation);
  if (vec.empty()) {
    return nullptr;
  }
  return static_cast<const StdInfoType<S>*>(vec.front().get());
}

// Reads one flag of $STANDARD_INFORMATION. False if the record has none.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::StdInfoFlag(bool (StdInfoType<S>::*flag)()
                                              const noexcept) const noexcept {
  const StdInfoType<S>* std_info = StdInfo();
  return std_info != nullptr && (std_info->*flag)();
}

// The body of ParseFileRecord(), shared with callers that already hold the
// record's bytes.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseRecord(ULONGLONG file_ref,
                                          std::span<const BYTE> preread) {
  // Clear previous data
  ClearAttrs();
  file_record.reset();
  reused_extensions.clear();
  overwritten_list = false;

  typename RecordLoader<S>::Loaded loaded =
      RecordLoader<S>(*volume, record_buffer, record_source)
          .Load(file_ref, preread);
  failed_fixup = loaded.failed_fixup;
  header_salvaged = loaded.salvaged;
  if (loaded.read_ok || loaded.accepted) {
    file_reference = file_ref;
  } else {
    file_reference = {};
  }
  file_record = std::move(loaded.header);
  return loaded.accepted;
}

template <Cache::Strategy S>
Io::FileRecord<S>::FileRecord(const NtfsVolume<S>& volume)
    : impl_(std::make_unique<Impl>(*this, volume)) {}

template <Cache::Strategy S>
Io::FileRecord<S>::FileRecord(Io::FileRecord<S>&& other) noexcept
    : impl_(std::move(other.impl_)) {
  impl_->self = this;
}

template <Cache::Strategy S>
Io::FileRecord<S>::~FileRecord() = default;

template <Cache::Strategy S>
const NtfsVolume<S>& Io::FileRecord<S>::GetVolume() const noexcept {
  return *impl_->volume;
}

// Drops every parsed attribute, then the records and copies they point into.
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ClearAttrs() noexcept {
  for (std::vector<std::unique_ptr<Attr::AttrBase<S>>>& arr : attr_list) {
    arr.clear();
  }
  // Only now: attributes imported from these records are gone.
  extension_records.clear();
  realigned_attrs.clear();
  end_marker_offset.reset();
  attrs_salvaged = false;
  claimed_layouts.clear();
}

// Call user defined Callback routines for an attribute
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::UserCallBack(DWORD att_type,
                                           const Attr::HeaderCommon& ahc,
                                           bool& discard) {
  discard = false;

  if (attr_raw_call_back.at(att_type) != nullptr) {
    attr_raw_call_back.at(att_type)(ahc, discard);
  } else {
    volume->DispatchAttrRawCB(att_type, ahc, discard);
  }
}

// Read File Record, verify and patch the US (update sequence)
template <Cache::Strategy S>
bool Io::FileRecord<S>::ParseFileRecord(ULONGLONG file_ref) {
  return impl_->ParseRecord(file_ref, {});
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::ParseFileRecord(ULONGLONG file_ref,
                                        std::span<const BYTE> preread) {
  return impl_->ParseRecord(file_ref, preread);
}

// Parse all the attributes in a File Record
// And insert them into a link list
template <Cache::Strategy S>
bool Io::FileRecord<S>::ParseAttrs() {
  // A fresh chain, unrelated to any previous ParseFileRecord() on this object.
  impl_->attr_list_chain.clear();
  return impl_->ParseAttrs(impl_->attr_list_chain);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::ClearAttrs() noexcept {
  impl_->ClearAttrs();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::HasFailedFixup() const noexcept {
  return impl_->failed_fixup;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSalvaged() const noexcept {
  return impl_->header_salvaged || impl_->attrs_salvaged;
}

template <Cache::Strategy S>
ULONGLONG Io::FileRecord<S>::GetReusedExtensionCount() const noexcept {
  return impl_->reused_extensions.size();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::HasOverwrittenAttrList() const noexcept {
  return impl_->overwritten_list;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::NoteOverwrittenAttrList() {
  if (impl_->overwritten_list) {
    return;
  }
  impl_->overwritten_list = true;
  Log::Debug(
      "Attribute List: the list of freed record {} was overwritten by another "
      "file - dropping the rest of it",
      impl_->file_reference.value_or(0));
}

template <Cache::Strategy S>
std::optional<ULONGLONG> Io::FileRecord<S>::GetFileReference() const noexcept {
  return impl_->file_reference;
}

template <Cache::Strategy S>
WORD Io::FileRecord<S>::GetSequenceNumber() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->seq_no : 0;
}

template <Cache::Strategy S>
ULONGLONG Io::FileRecord<S>::GetLogSequenceNumber() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->lsn : 0;
}

template <Cache::Strategy S>
ULONGLONG Io::FileRecord<S>::GetBaseRecordReference() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->ref_to_base &
                                  Mft::mft_record_number_mask
                            : 0;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsExtensionRecord() const noexcept {
  return impl_->file_record && impl_->file_record->GetData()->ref_to_base != 0;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::HasAttrHeader(Attr::Type attr_type) const noexcept {
  return impl_->file_record &&
         RawAttrHeaders(*impl_->file_record->GetData(),
                        impl_->volume->GetFileRecordSize())
             .Has(attr_type);
}

template <Cache::Strategy S>
std::vector<Alloc::Extent>
    Io::FileRecord<S>::AttributeListExtents(ULONGLONG max_bytes) const {
  if (!impl_->file_record) {
    return {};
  }
  return RawAttrHeaders(*impl_->file_record->GetData(),
                        impl_->volume->GetFileRecordSize())
      .AttributeListExtents(impl_->volume->GetClusterSize(), max_bytes);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetExtensionSource(RecordSource source) {
  impl_->extension_source = std::move(source);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetBaseSource(BaseSource source) {
  impl_->base_source = std::move(source);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetRecordSource(RecordSource source) {
  impl_->record_source = std::move(source);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetIndexBlockSource(IndexBlockSource source) {
  impl_->index_block_source = std::move(source);
}

template <Cache::Strategy S>
const typename Io::FileRecord<S>::IndexBlockSource&
    Io::FileRecord<S>::GetIndexBlockSource() const noexcept {
  return impl_->index_block_source;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetProbeStore(ProbeStore store) {
  impl_->probe_store = std::move(store);
}

template <Cache::Strategy S>
const typename Io::FileRecord<S>::ProbeStore&
    Io::FileRecord<S>::GetProbeStore() const noexcept {
  return impl_->probe_store;
}

// Install Attribute raw data CallBack routines for a single File Record
template <Cache::Strategy S>
bool Io::FileRecord<S>::InstallAttrRawCB(Attr::Type attr_type,
                                         Attr::RawCallback callback) noexcept {
  const DWORD at_idx = Attr::AttrIndex(attr_type);
  if (at_idx >= Attr::attr_nums) {
    return false;
  }

  // atIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_->attr_raw_call_back[at_idx] = callback;
  return true;
}

// Clear all Attribute CallBack routines
template <Cache::Strategy S>
void Io::FileRecord<S>::ClearAttrRawCB() noexcept {
  for (Attr::RawCallback& callback : impl_->attr_raw_call_back) {
    callback = nullptr;
  }
}

template <Cache::Strategy S>
void Io::FileRecord<S>::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover) {
  typename Impl::ExtensionImporter importer(*impl_, attr_list_chain, recover);
  importer.Import(record_number, sequence_number, attr_type);
}

// Choose attributes to handle, unwanted attributes will be discarded silently
template <Cache::Strategy S>
void Io::FileRecord<S>::SetAttrMask(Attr::Mask mask) noexcept {
  // Standard Information and Attribute List is needed always
  impl_->attr_mask =
      mask | Attr::Mask::StandardInformation | Attr::Mask::AttributeList;

  // The $EFS stream holds the key of every encrypted $DATA.
  if ((mask & Attr::Mask::Data) == Attr::Mask::Data) {
    impl_->attr_mask |= Attr::Mask::LoggedUtilityStream;
  }
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetClaimMask(Attr::Mask mask) noexcept {
  impl_->claim_mask = mask;
}

template <Cache::Strategy S>
const std::vector<Alloc::StreamLayout>&
    Io::FileRecord<S>::GetClaimedLayouts() const noexcept {
  return impl_->claimed_layouts;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetExactAttrMask(Attr::Mask mask) noexcept {
  impl_->attr_mask = mask;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetBypassDeletedGate(bool bypass) noexcept {
  impl_->bypass_deleted_gate = bypass;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::BypassesDeletedGate() const noexcept {
  return impl_->bypass_deleted_gate;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetResolveAttrList(bool resolve) noexcept {
  impl_->resolve_attr_list = resolve;
}

template <Cache::Strategy S>
void Io::FileRecord<S>::SetReadRefusal(bool* refused) noexcept {
  impl_->read_refusal = refused;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::RefuseRead() const noexcept {
  if (impl_->read_refusal == nullptr) {
    return false;
  }
  *impl_->read_refusal = true;
  return true;
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
template <Cache::Strategy S>
void Io::FileRecord<S>::TraverseAttrs(const AttrsCallback& attr_call_back,
                                      void* context) {
  if (!attr_call_back) {
    Log::Warn("TraverseAttrs() called with an empty callback");
    return;
  }

  for (size_t i = 0; i < Attr::attr_nums; i++) {
    // skip masked attributes
    if (static_cast<bool>(impl_->attr_mask &
                          (static_cast<Attr::Mask>(1U << i)))) {
      // i < attr_nums by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      for (const std::unique_ptr<Attr::AttrBase<S>>& attr_base :
           impl_->attr_list[i]) {
        bool stop = false;
        attr_call_back(*attr_base.get(), context, &stop);
        if (stop) {
          return;
        }
      }
    }
  }
}

// Find Attributes
template <Cache::Strategy S>
const std::vector<std::unique_ptr<Attr::AttrBase<S>>>&
    Io::FileRecord<S>::GetAttr(Attr::Type attr_type) const noexcept {
  static const std::vector<std::unique_ptr<Attr::AttrBase<S>>> dummy{};
  const DWORD attr_idx = Attr::AttrIndex(attr_type);

  if (attr_idx >= Attr::attr_nums) {
    return dummy;
  }

  // attrIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->attr_list[attr_idx];
}

template <Cache::Strategy S>
std::vector<std::unique_ptr<Attr::AttrBase<S>>>&
    Io::FileRecord<S>::GetAttr(Attr::Type attr_type) noexcept {
  static std::vector<std::unique_ptr<Attr::AttrBase<S>>> dummy{};
  const DWORD attr_idx = Attr::AttrIndex(attr_type);

  if (attr_idx >= Attr::attr_nums) {
    return dummy;
  }

  // attrIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->attr_list[attr_idx];
}

// Get File Name (First Win32 name)
template <Cache::Strategy S>
std::wstring_view Io::FileRecord<S>::GetFileName() const {
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<Attr::AttrBase<S>>& fn :
       std::get<Attr::AttrIndex(Attr::Type::FileName)>(impl_->attr_list)) {
    const Filename* filename =
        static_cast<const Attr::AttrFileName<ResidentOf<S>, S>*>(fn.get());
    if (filename->IsWin32Name() && !filename->GetFilename().empty()) {
      return filename->GetFilename();
    }
  }

  return {};
}

// Get File Size
template <Cache::Strategy S>
ULONGLONG Io::FileRecord<S>::GetFileSize() const noexcept {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      std::get<Attr::AttrIndex(Attr::Type::FileName)>(impl_->attr_list);
  if (vec.empty()) {
    return 0;
  }
  return static_cast<const Attr::AttrFileName<ResidentOf<S>, S>*>(
             vec.front().get())
      ->GetFileSize();
}

template <Cache::Strategy S>
ULONGLONG Io::FileRecord<S>::GetAllocatedSize() const noexcept {
  const Attr::AttrBase<S>* data = FindStream({});
  return data != nullptr ? data->GetAllocatedSize() : 0;
}

// Get File Times
template <Cache::Strategy S>
void Io::FileRecord<S>::GetFileTime(FILETIME* write_tm, FILETIME* create_tm,
                                    FILETIME* access_tm,
                                    FILETIME* change_tm) const noexcept {
  // Standard Information attribute hold the most updated file time
  if (const auto* std_info = impl_->StdInfo(); std_info != nullptr) {
    std_info->GetFileTime(write_tm, create_tm, access_tm, change_tm);
    return;
  }

  if (write_tm != nullptr) {
    write_tm->dwHighDateTime = 0;
    write_tm->dwLowDateTime = 0;
  }
  if (create_tm != nullptr) {
    create_tm->dwHighDateTime = 0;
    create_tm->dwLowDateTime = 0;
  }
  if (access_tm != nullptr) {
    access_tm->dwHighDateTime = 0;
    access_tm->dwLowDateTime = 0;
  }
  if (change_tm != nullptr) {
    change_tm->dwHighDateTime = 0;
    change_tm->dwLowDateTime = 0;
  }
}

template <Cache::Strategy S>
std::vector<Record::CarvedName> Io::FileRecord<S>::CarveSlackNames() const {
  return SlackNameCarver<S>(*this, impl_->file_reference)
      .FromRecord(impl_->file_record.get(), impl_->end_marker_offset);
}

template <Cache::Strategy S>
std::vector<Record::CarvedName>
    Io::FileRecord<S>::CarveIndexSlackNames() const {
  return SlackNameCarver<S>(*this, impl_->file_reference).FromIndex();
}

template <Cache::Strategy S>
void Io::FileRecord<S>::TraverseSubEntries(const SubentryCallback& se_call_back,
                                           void* context) const {
  DirectoryIndex<S>(*this).Traverse(se_call_back, context);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::TraverseSubEntriesWithStanding(
    const StandingCallback& se_call_back, void* context) const {
  DirectoryIndex<S>(*this).TraverseWithStanding(se_call_back, context);
}

// Find a specific Filename from InexRoot described B+ tree
template <Cache::Strategy S>
std::optional<IndexEntry>
    Io::FileRecord<S>::FindSubEntry(std::wstring_view file_name) const {
  // Loaded before the walk: reading $UpCase reuses the volume's buffers.
  const UpCase::Table& upcase = impl_->volume->GetUpCaseTable();
  return DirectoryIndex<S>(*this).Find(file_name, upcase);
}

// Find Data attribute class of
template <Cache::Strategy S>
const Attr::AttrBase<S>*
    Io::FileRecord<S>::FindStream(std::wstring_view name) const {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      GetAttr(Attr::Type::Data);
  for (const std::unique_ptr<Attr::AttrBase<S>>& data : vec) {
    // Unnamed stream
    if (data->IsUnNamed() && name.empty()) {
      Log::Debug("FindStream() found the unnamed stream");
      return data.get();
    }
    // Named stream
    if ((!data->IsUnNamed()) && data->GetAttrName() == name) {
      Log::Debug("FindStream() found stream named \"{}\"",
                 Utf::WideToUtf8(name));
      return data.get();
    }
  }

  Log::Debug("FindStream() found no stream named \"{}\"",
             Utf::WideToUtf8(name));
  return nullptr;
}

// Judges a stream by asking the volume's cluster bitmap about each extent. A
// live record owns its clusters by definition, so it is not asked.
template <Cache::Strategy S>
std::optional<Alloc::DataState>
    Io::FileRecord<S>::GetDataState(std::wstring_view stream_name) const {
  const Attr::AttrBase<S>* stream = FindStream(stream_name);
  if (stream == nullptr) {
    return std::nullopt;
  }
  if (!IsDeleted()) {
    return Alloc::DataState::Intact;
  }

  const Alloc::StreamExtents extents = stream->GetExtents();
  std::vector<Alloc::RangeQuery> queries;
  queries.reserve(extents.extents.size());
  for (const Alloc::Extent& extent : extents.extents) {
    queries.push_back({.lcn = extent.lcn, .clusters = extent.clusters});
  }
  impl_->volume->GetRangeStates(queries);

  std::vector<Alloc::ClusterState> states;
  states.reserve(queries.size());
  for (const Alloc::RangeQuery& query : queries) {
    states.push_back(query.state);
  }
  return Alloc::FoldDataState(extents.complete, states);
}

// Check if it's deleted or in use
template <Cache::Strategy S>
bool Io::FileRecord<S>::IsDeleted() const noexcept {
  if (!impl_->file_record) {
    Log::Warn("IsDeleted() called on a FileRecord with no parsed record");
    return false;
  }

  return !static_cast<bool>(impl_->file_record->GetData()->flags &
                            Data::FileRecordFlag::InUse);
}

// Check if it's a directory
template <Cache::Strategy S>
bool Io::FileRecord<S>::IsDirectory() const noexcept {
  if (!impl_->file_record) {
    Log::Warn("IsDirectory() called on a FileRecord with no parsed record");
    return false;
  }

  return static_cast<bool>(impl_->file_record->GetData()->flags &
                           Data::FileRecordFlag::Dir);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsReadOnly() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsReadOnly);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsHidden() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsHidden);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSystem() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsSystem);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsArchive() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsArchive);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsDevice() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsDevice);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsNormal() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsNormal);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsTemporary() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsTemporary);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsCompressed() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsCompressed);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsOffline() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsOffline);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsNotContentIndexed() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsNotContentIndexed);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsEncrypted() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsEncrypted);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSparse() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsSparse);
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsReparsePoint() const noexcept {
  return impl_->StdInfoFlag(&StdInfoType<S>::IsReparsePoint);
}

template class FileRecord<Cache::Strategy::NoCache>;
template class FileRecord<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
