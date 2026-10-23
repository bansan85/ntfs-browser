#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc/data-state.h"
#include "attr/file-name.h"
#include "attr/list-chain.h"
#include "attr/slot.h"
#include "attr/std-info.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "io/file-record-attr-types.h"
#include "io/file-record-impl.h"
#include "io/file-record-index.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/record-offset.h"
#include "ntfs-browser/record/carved-name.h"
#include "ntfs-browser/volume-options.h"
#include "record/slack-carver.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Attr {

struct HeaderCommon;
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

// Byte offset of record file_ref when $MFT is taken as one continuous disk
// run. Empty when the offset does not fit a LONGLONG.
template <Cache::Strategy S>
std::optional<LONGLONG> DirectRecordAddress(const NtfsVolume<S>& volume,
                                            ULONGLONG file_ref) {
  const std::optional<ULONGLONG> offset =
      Mft::RecordOffset(volume.GetFileRecordSize(), file_ref);
  if (!offset) {
    return std::nullopt;
  }

  try {
    return gsl::narrow<LONGLONG>(volume.GetMFTAddr() + *offset);
  } catch (const std::exception& e) {
    // fileRef is attacker-controlled and unbounded, so this sum can
    // still overflow a LONGLONG even with mft_addr_ validated.
    Log::Exception(e);
    return std::nullopt;
  }
}

// What the checks on a record just read decided.
enum class HeaderVerdict : BYTE {
  Rejected,  // The record is unusable.
  Salvaged,  // Recovering: some sector fails its check, the others verify.
  Accepted   // The record is whole.
};

// Checks the magic of a record just read, and patches its update sequence.
// Strict rejects any sector that fails. Recovering patches the sectors that
// verify and salvages the record when at least one does: with none, nothing
// shows the bytes belong to this record. A record that does not carry the FILE
// magic (a BAAD one, or zeros) is never salvaged. fixup_failed is set when a
// sector failed its check, whatever the verdict.
template <Cache::Strategy S>
HeaderVerdict JudgeRecordHeader(Record::HeaderImpl<S>& header, bool recover,
                                bool& fixup_failed) {
  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets Rejected either way.
  if (header.GetData()->magic != Data::FileRecordHeader::file_record_magic) {
    Log::Debug("Invalid file record");
    return HeaderVerdict::Rejected;
  }

  if (!recover) {
    if (!header.PatchUS()) {
      fixup_failed = true;
      Log::Warn("Update Sequence Number error");
      return HeaderVerdict::Rejected;
    }
    return HeaderVerdict::Accepted;
  }

  const size_t failed = header.PatchUSSalvage();
  if (failed == 0) {
    return HeaderVerdict::Accepted;
  }
  fixup_failed = true;
  if (failed >= header.SectorCount()) {
    Log::Warn("Update Sequence Number error: no sector of the record verifies");
    return HeaderVerdict::Rejected;
  }
  Log::Recoverable(
      true,
      "Update Sequence Number error: {} of {} sectors do not verify, the "
      "record is salvaged",
      failed, header.SectorCount());
  return HeaderVerdict::Salvaged;
}

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
  end_marker_offset.reset();
  attrs_salvaged = false;
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

// Reads file record file_ref straight from disk, at a fixed offset from
// mft_addr, into record_buffer. False if the read fails.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ReadDirectRecord(ULONGLONG file_ref) {
  const std::optional<LONGLONG> addr = DirectRecordAddress(*volume, file_ref);
  if (!addr) {
    return false;
  }

  LARGE_INTEGER fr_addr{};
  fr_addr.QuadPart = *addr;
  return volume->ReadInto(fr_addr, record_buffer);
}

// Parses the header of the record held in record_buffer. Null when the header
// constructor rejects it, as it does a forged record.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>> Io::FileRecord<S>::Impl::ParseHeader() {
  try {
    return std::make_unique<Record::HeaderImpl<S>>(record_buffer);
  } catch (const std::exception& e) {
    Log::Exception(e);
    return {};
  }
}

// Reads file record fileRef into record_buffer_ and returns its parsed
// header. Early records (and any record read before $MFT's own DATA
// attribute is known) come straight from disk at a fixed offset; later
// records go through $MFT's DATA attribute, since $MFT itself may be
// fragmented across the disk. A non-empty preread replaces the read.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    Io::FileRecord<S>::Impl::ReadFileRecord(ULONGLONG file_ref,
                                            std::span<const BYTE> preread) {
  if (record_buffer.size() != volume->GetFileRecordSize()) {
    record_buffer.resize(volume->GetFileRecordSize());
  }

  if (!preread.empty()) {
    // The header patches the update sequence in place, so it works on a copy.
    if (preread.size() != record_buffer.size()) {
      return {};
    }
    std::ranges::copy(preread, record_buffer.begin());
  } else if (file_ref < static_cast<ULONGLONG>(Mft::Idx::User) ||
             !volume->HasMftBase()) {
    if (!ReadDirectRecord(file_ref)) {
      return {};
    }
  } else {
    // May be fragmented $MFT, and its DATA attribute itself may be split
    // across extension records - ReadData() picks whichever instance
    // covers this offset.
    const std::optional<ULONGLONG> fr_addr =
        Mft::RecordOffset(volume->GetFileRecordSize(), file_ref);
    if (!fr_addr) {
      return {};
    }

    if (std::optional<ULONGLONG> len =
            volume->ReadMftData(*fr_addr, record_buffer);
        !len || *len != volume->GetFileRecordSize()) {
      return {};
    }
  }

  return ParseHeader();
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

// Reads the $MFTMirr copy of file_ref into record_buffer. Null when there is
// no copy to read, or it has no usable header.
template <Cache::Strategy S>
std::unique_ptr<Record::HeaderImpl<S>>
    Io::FileRecord<S>::Impl::ReadMirrorRecord(ULONGLONG file_ref) {
  if (!volume->ReadMftMirrorRecord(file_ref, record_buffer)) {
    return {};
  }

  return ParseHeader();
}

// The body of ParseFileRecord(), shared with callers that already hold the
// record's bytes.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseRecord(ULONGLONG file_ref,
                                          std::span<const BYTE> preread) {
  // Clear previous data
  ClearAttrs();
  if (file_record) {
    file_record.reset();
  }

  const bool recover = volume->GetOptions().recover_errors;
  failed_fixup = false;
  header_salvaged = false;

  std::unique_ptr<Record::HeaderImpl<S>> header =
      ReadFileRecord(file_ref, preread);
  const bool read_ok = header != nullptr;
  if (!read_ok) {
    Log::Error("Cannot read file record {}", file_ref);
  }
  HeaderVerdict verdict =
      read_ok ? JudgeRecordHeader(*header, recover, failed_fixup)
              : HeaderVerdict::Rejected;

  // The mirror is the last resort: it may be older than $MFT's own record, as
  // NTFS refreshes it only now and then. The volume reads it only when
  // recovering, and only for the first four records. A caller that supplies
  // the bytes has no use for another source. It beats a salvaged record, whose
  // sectors are partly unverified.
  if (verdict != HeaderVerdict::Accepted && preread.empty()) {
    header.reset();
    header = ReadMirrorRecord(file_ref);
    bool mirror_fixup_failed = false;
    if (header && JudgeRecordHeader(*header, recover, mirror_fixup_failed) ==
                      HeaderVerdict::Accepted) {
      verdict = HeaderVerdict::Accepted;
      // The record of $MFT was damaged, and a copy stands in for it.
      header_salvaged = failed_fixup;
      Log::Info(
          "File record {} is unusable in $MFT: using its copy in $MFTMirr, "
          "which may be stale",
          file_ref);
    } else if (verdict == HeaderVerdict::Salvaged) {
      // The read of the mirror, if there was one, overwrote the record.
      header = ReadFileRecord(file_ref, preread);
      bool again_failed = false;
      verdict = header ? JudgeRecordHeader(*header, recover, again_failed)
                       : HeaderVerdict::Rejected;
    }
  }
  const bool accepted = verdict != HeaderVerdict::Rejected;
  header_salvaged = header_salvaged || verdict == HeaderVerdict::Salvaged;

  if (read_ok || accepted) {
    file_reference = file_ref;
  } else {
    file_reference = {};
  }
  if (!accepted) {
    return false;
  }

  Log::Debug("File Record {} Found", file_ref);
  file_record = std::move(header);

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

template <Cache::Strategy S>
bool Io::FileRecord<S>::HasFailedFixup() const noexcept {
  return impl_->failed_fixup;
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSalvaged() const noexcept {
  return impl_->header_salvaged || impl_->attrs_salvaged;
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

template <Cache::Strategy S>
void Io::FileRecord<S>::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover) {
  impl_->ImportAttrListEntry(record_number, sequence_number, attr_type,
                             attr_list_chain, recover);
}

template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ImportAttrListEntry(
    ULONGLONG record_number, WORD sequence_number, Attr::Type attr_type,
    std::unordered_set<ULONGLONG>& attr_list_chain, bool recover) {
  const Attr::Mask entry_mask = Attr::AttrMask(attr_type);
  // Set by ParseRecord(), which every attribute parse follows.
  const ULONGLONG self_ref = file_reference.value_or(0);
  // Skip contained attributes
  // Skip unwanted attributes
  if (record_number == self_ref || !static_cast<bool>(entry_mask & attr_mask)) {
    return;
  }

  if (!attr_list_chain.insert(Attr::MakeChainKey(record_number, attr_type))
           .second) {
    Log::Info(
        "Attribute List: record {}, type 0x{:04x} already resolved in "
        "this chain, skipping",
        record_number, static_cast<DWORD>(attr_type));
    return;
  }

  // Owned by this record, not by the caller: the attributes moved into it
  // below keep pointing into frnew's bytes.
  extension_records.emplace_back(*volume);
  Io::FileRecord<S>& frnew = extension_records.back();

  frnew.impl_->attr_mask = entry_mask;
  frnew.impl_->attr_raw_call_back = attr_raw_call_back;
  if (!frnew.ParseFileRecord(record_number)) {
    throw std::runtime_error("Attribute List parse error (ParseFileRecord).\n");
  }

  // A record another file reused since the list was written is not
  // this file's extension: its attributes belong to someone else.
  const bool genuine = Mft::IsGenuineExtensionRecord(
      sequence_number, frnew.GetSequenceNumber(),
      frnew.GetBaseRecordReference(), self_ref & Mft::mft_record_number_mask);
  if (!genuine) {
    extension_records.pop_back();
    Log::Recoverable(recover,
                     "Attribute List: record {} is not an extension of "
                     "record {} (reused or foreign) - skipping",
                     record_number, self_ref);
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
      frnew.GetAttr(attr_type);
  for (std::unique_ptr<Attr::AttrBase<S>>& veci : vec) {
    attr_list.at(Attr::AttrIndex(attr_type)).push_back(std::move(veci));
  }
  vec.clear();
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
  return impl_->CarveSlackNames();
}

// The carvers report UTC, as stored. The times of the file getters are local.
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::ConvertCarvedTimes(
    std::vector<Record::CarvedName>& carved) {
  const auto to_local = [](FILETIME& time) {
    constexpr unsigned dword_bits = 32;
    const ULONGLONG utc =
        (static_cast<ULONGLONG>(time.dwHighDateTime) << dword_bits) |
        time.dwLowDateTime;
    StdInfoType<S>::UTC2Local(utc, time);
  };
  for (Record::CarvedName& name : carved) {
    // Unknown times stay zero.
    if (!name.times_known) {
      continue;
    }
    to_local(name.create_time);
    to_local(name.write_time);
    to_local(name.change_time);
    to_local(name.access_time);
  }
}

template <Cache::Strategy S>
std::vector<Record::CarvedName>
    Io::FileRecord<S>::CarveIndexSlackNames() const {
  std::vector<Record::CarvedName> carved =
      DirectoryIndex<S>(*this).CarveSlackNames();
  Impl::ConvertCarvedTimes(carved);
  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the index slack",
               impl_->file_reference.value_or(0), carved.size());
  }
  return carved;
}

// Carves the bytes after the end marker, patched like the rest of the record,
// then drops the names this record also holds live.
template <Cache::Strategy S>
std::vector<Record::CarvedName>
    Io::FileRecord<S>::Impl::CarveSlackNames() const {
  const VolumeOptions& options = volume->GetOptions();
  if (!options.recover_errors || !options.include_deleted || !file_record ||
      !end_marker_offset) {
    return {};
  }

  const std::span<const BYTE> bytes(file_record->GetData()->raw,
                                    file_record->buffer_size);
  std::vector<Record::CarvedName> carved =
      Record::CarveFileNames(bytes, *end_marker_offset);

  const auto& live_names =
      std::get<Attr::AttrIndex(Attr::Type::FileName)>(attr_list);
  std::erase_if(carved, [&live_names](const Record::CarvedName& candidate) {
    return std::ranges::any_of(
        live_names, [&candidate](const std::unique_ptr<Attr::AttrBase<S>>& fn) {
          const Filename& live =
              *static_cast<const Attr::AttrFileName<ResidentOf<S>, S>*>(
                  fn.get());
          return live.GetFilename() == candidate.name &&
                 (!candidate.parent_known ||
                  live.GetParentReference() == candidate.parent_record);
        });
  });

  ConvertCarvedTimes(carved);

  if (!carved.empty()) {
    Log::Debug("File record {}: {} name(s) carved from the slack",
               file_reference.value_or(0), carved.size());
  }
  return carved;
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
