#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/file-name.h"
#include "attr/std-info.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "io/file-record-attrs.h"
#include "io/file-record-impl.h"
#include "io/file-record-index.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "ntfs-volume-impl.h"
#include "record/header.h"
#include "upcase/upcase.h"
#include "utf/utf.h"

namespace NtfsBrowser::Io {

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
    volume->impl_->AttrRawCallBack(att_type, ahc, discard);
  }
}

// Reads file record fileRef into record_buffer_ and returns its parsed
// header. Early records (and any record read before $MFT's own DATA
// attribute is known) come straight from disk at a fixed offset; later
// records go through $MFT's DATA attribute, since $MFT itself may be
// fragmented across the disk.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    Io::FileRecord<S>::Impl::ReadFileRecord(ULONGLONG file_ref) {
  if (record_buffer.size() != volume->GetFileRecordSize()) {
    record_buffer.resize(volume->GetFileRecordSize());
  }

  if (file_ref < static_cast<ULONGLONG>(Mft::Idx::User) ||
      volume->impl_->mft_data == nullptr) {
    // Take as continuous disk allocation
    LARGE_INTEGER fr_addr{};
    try {
      fr_addr.QuadPart = gsl::narrow<LONGLONG>(
          volume->GetMFTAddr() + volume->GetFileRecordSize() * file_ref);
    } catch (const std::exception& e) {
      // fileRef is attacker-controlled and unbounded, so this sum can
      // still overflow a LONGLONG even with mft_addr_ validated.
      Log::Exception(e);
      return {};
    }

    if (!volume->ReadInto(fr_addr, record_buffer)) {
      return {};
    }

    try {
      return std::make_unique<Record::HeaderImpl<S>>(record_buffer);
    } catch (const std::exception& e) {
      Log::Exception(e);
      return {};
    }
  }

  // May be fragmented $MFT, and its DATA attribute itself may be split
  // across extension records - ReadMftData() picks whichever instance
  // covers this offset.
  const ULONGLONG fr_addr = volume->GetFileRecordSize() * file_ref;

  if (std::optional<ULONGLONG> len =
          volume->impl_->ReadMftData(fr_addr, record_buffer);
      !len || *len != volume->GetFileRecordSize()) {
    return {};
  }

  try {
    return std::make_unique<Record::HeaderImpl<S>>(record_buffer);
  } catch (const std::exception& e) {
    // Reachable through the same constructor call as the direct-allocation
    // path above.
    Log::Exception(e);
    return {};
  }
}

// Read File Record, verify and patch the US (update sequence)
template <Cache::Strategy S>
bool Io::FileRecord<S>::ParseFileRecord(ULONGLONG file_ref) {
  // Clear previous data
  impl_->ClearAttrs();
  if (impl_->file_record) {
    impl_->file_record.reset();
  }

  std::unique_ptr<Record::HeaderImpl<S>> header =
      impl_->ReadFileRecord(file_ref);
  if (!header) {
    Log::Error("Cannot read file record {}", file_ref);

    impl_->file_reference = {};

    return false;
  }

  impl_->file_reference = file_ref;

  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets false either way.
  if (header->GetData()->magic != Data::FileRecordHeader::file_record_magic) {
    Log::Debug("Invalid file record");
    return false;
  }

  if (!header->PatchUS()) {
    Log::Warn("Update Sequence Number error");
    return false;
  }

  Log::Debug("File Record {} Found", file_ref);
  impl_->file_record = std::move(header);

  return true;
}

// Parse all the attributes in a File Record
// And insert them into a link list
template <Cache::Strategy S>
bool Io::FileRecord<S>::ParseAttrs() {
  // A fresh chain, unrelated to any previous ParseFileRecord() on this object.
  std::unordered_set<ULONGLONG> attr_list_chain;
  return impl_->ParseAttrs(attr_list_chain);
}

// Parses the attributes of this record, see AttrParser<S>::Parse().
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseAttrs(
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  return AttrParser<S>(*this).Parse(attr_list_chain);
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
ULONGLONG Io::FileRecord<S>::GetBaseRecordReference() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->ref_to_base &
                                  Mft::mft_record_number_mask
                            : 0;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsExtensionRecord() const noexcept {
  return impl_->file_record && impl_->file_record->GetData()->ref_to_base != 0;
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
void Io::FileRecord<S>::TraverseSubEntries(const SubentryCallback& se_call_back,
                                           void* context) const {
  DirectoryIndex<S>(*this).Traverse(se_call_back, context);
}

// Find a specific Filename from InexRoot described B+ tree
template <Cache::Strategy S>
std::optional<IndexEntry>
    Io::FileRecord<S>::FindSubEntry(std::wstring_view file_name) const {
  // Loaded before the walk: reading $UpCase reuses the volume's buffers.
  const UpCase::Table& upcase = impl_->volume->impl_->GetUpCaseTable();
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

}  // namespace NtfsBrowser::Io
