#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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

#include "attr/bitmap.h"  // IWYU pragma: keep
#include "attr/data.h"    // IWYU pragma: keep
#include "attr/file-name.h"
#include "attr/index-alloc.h"  // IWYU pragma: keep
#include "attr/index-root.h"
#include "attr/list.h"          // IWYU pragma: keep
#include "attr/non-resident.h"  // IWYU pragma: keep
#include "attr/resident.h"
#include "attr/slot.h"
#include "attr/std-info.h"
#include "attr/vol-info.h"  // IWYU pragma: keep
#include "attr/vol-name.h"  // IWYU pragma: keep
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "io/file-record-efs.h"
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

// Rejects an attribute of a type that is always resident on disk but claims
// to be non-resident, before its bytes get reinterpreted as a resident one.
void RequireResident(const Attr::HeaderCommon& ahc, const char* message) {
  if (ahc.non_resident != 0) {
    throw std::runtime_error(message);
  }
}

// Wraps ahc in Wrapper<Base, S>. Args are passed on after the record.
template <template <typename, Cache::Strategy> class Wrapper, typename Base,
          Cache::Strategy S, typename... Args>
std::unique_ptr<Attr::AttrBase<S>> MakeAttr(const Attr::HeaderCommon& ahc,
                                            FileRecord<S>& record,
                                            Args&&... args) {
  return std::make_unique<Wrapper<Base, S>>(ahc, record,
                                            std::forward<Args>(args)...);
}

// Wraps a type that is always resident on disk, after rejecting a
// non-resident header with message.
template <template <typename, Cache::Strategy> class Wrapper, typename Resident,
          Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>>
    MakeResidentOnly(const Attr::HeaderCommon& ahc, FileRecord<S>& record,
                     const char* message) {
  RequireResident(ahc, message);
  return MakeAttr<Wrapper, Resident>(ahc, record);
}

// Wraps the non-resident form of a type when the header is non-resident,
// else its resident form. Args are passed on after the record.
template <template <typename, Cache::Strategy> class Wrapper, typename Resident,
          Cache::Strategy S, typename... Args>
std::unique_ptr<Attr::AttrBase<S>>
    MakeByResidency(const Attr::HeaderCommon& ahc, FileRecord<S>& record,
                    Args&&... args) {
  if (ahc.non_resident != 0) {
    return MakeAttr<Wrapper, Attr::AttrNonResident<S>>(
        ahc, record, std::forward<Args>(args)...);
  }
  return MakeAttr<Wrapper, Resident>(ahc, record, std::forward<Args>(args)...);
}

// Wraps a type with no class of its own, as its non-resident or resident base.
template <typename Resident, Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>> MakeGeneric(const Attr::HeaderCommon& ahc,
                                               FileRecord<S>& record) {
  if (ahc.non_resident != 0) {
    return std::make_unique<Attr::AttrNonResident<S>>(ahc, record);
  }
  return std::make_unique<Resident>(ahc, record);
}

// Wraps INDEX_ALLOCATION, which is always non-resident on disk. Rejects a
// resident header first, before its bytes get reinterpreted as one.
template <Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>> MakeIndexAlloc(const Attr::HeaderCommon& ahc,
                                                  FileRecord<S>& record) {
  if (ahc.non_resident == 0) {
    throw std::runtime_error(
        "Index Allocation attribute must be non-resident.\n");
  }
  return std::make_unique<Attr::AttrIndexAlloc<S>>(ahc, record);
}

// How reading the next attribute header of a record's walk ended.
enum class AttrWalk : BYTE {
  Next,       // head holds a header whose attribute fits in the record.
  EndMarker,  // The terminating Attr::Type::All marker was reached.
  RanOut,     // The record ended without a marker, or an attribute overran it.
  Abort       // The attribute is malformed.
};

// Reads the attribute header at the front of cur, and checks that it and its
// attribute fit in a record of recordSize bytes, dataPtr bytes into it.
AttrWalk ReadAttrHeader(std::span<const BYTE> cur, DWORD data_ptr,
                        DWORD record_size, Attr::HeaderCommon& head) {
  // The on-disk end-of-attributes marker is a single Attr::Type::All value
  // (4 bytes): check for it as soon as that much room remains, rather
  // than requiring a full attribute header to fit first.
  if (static_cast<ULONGLONG>(data_ptr) + sizeof(Attr::Type) > record_size) {
    return AttrWalk::RanOut;
  }
  Attr::Type type{};
  std::memcpy(&type, cur.data(), sizeof(type));
  if (type == Attr::Type::All) {
    return AttrWalk::EndMarker;
  }
  // From here on, the walk needs the whole header, and the whole
  // attribute, to fit.
  if (static_cast<ULONGLONG>(data_ptr) + sizeof(Attr::HeaderCommon) >
      record_size) {
    return AttrWalk::RanOut;
  }
  std::memcpy(&head, cur.data(), sizeof(head));
  if (static_cast<ULONGLONG>(data_ptr) + head.total_size > record_size) {
    return AttrWalk::RanOut;
  }

  const DWORD min_total_size =
      head.non_resident != 0 ? Data::header_non_resident_base_size
                             : static_cast<DWORD>(sizeof(Data::HeaderResident));
  if (head.total_size < min_total_size) {
    Log::Warn("Attribute total_size too small for its header.");
    return AttrWalk::Abort;
  }

  if (head.non_resident != 0) {
    Data::HeaderNonResident non_resident{};
    std::memcpy(&non_resident, cur.data(), sizeof(non_resident));
    if (Data::HasCompressedSizeField(non_resident) &&
        head.total_size < min_total_size + Data::compressed_size_field_size) {
      Log::Warn(
          "Compressed attribute total_size too small for its compressed "
          "size field.");
      return AttrWalk::Abort;
    }
  }
  return AttrWalk::Next;
}

// The resident base the attribute wrappers of strategy S are built over.
template <Cache::Strategy S>
using ResidentOf =
    std::conditional_t<S == Cache::Strategy::NoCache, Attr::AttrResidentNoCache,
                       Attr::AttrResidentFullCache>;

// Returns the record's $STANDARD_INFORMATION wrapper, or nullptr if it has
// none.
template <Cache::Strategy S>
const Attr::AttrStdInfo<ResidentOf<S>, S>*
    StdInfoOf(const FileRecord<S>& record) noexcept {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record.GetAttr(Attr::Type::StandardInformation);
  if (vec.empty()) {
    return nullptr;
  }
  return static_cast<const Attr::AttrStdInfo<ResidentOf<S>, S>*>(
      vec.front().get());
}

}  // namespace

template <Cache::Strategy S>
Io::FileRecord<S>::Impl::Impl(Io::FileRecord<S>& self,
                              const NtfsVolume<S>& volume) noexcept
    : self(&self), volume(&volume) {}

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

// Returns the attribute header at the start of `at`, the bytes from there to
// the end of the record: in place when it is aligned for the on-disk structs,
// else in an aligned copy of `at`. The copy keeps every read the attribute
// makes inside the record, as in place.
template <Cache::Strategy S>
const Attr::HeaderCommon&
    Io::FileRecord<S>::Impl::AlignedAttrHeader(std::span<const BYTE> bytes) {
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) %
          alignof(Data::HeaderNonResident) ==
      0) {
    return *reinterpret_cast<const Attr::HeaderCommon*>(bytes.data());
  }

  auto& copy = realigned_attrs.emplace_back(bytes.size());
  std::memcpy(copy.data(), bytes.data(), bytes.size());
  return *reinterpret_cast<const Attr::HeaderCommon*>(copy.data());
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

// Wraps one raw attribute in the class that matches its type. unhandled is
// set for a type this library has no wrapper for.
template <Cache::Strategy S>
template <typename Resident>
std::unique_ptr<Attr::AttrBase<S>> Io::FileRecord<S>::Impl::AllocAttr(
    const Attr::HeaderCommon& ahc, bool& unhandled,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  std::unique_ptr<Attr::AttrBase<S>> attr;
  switch (ahc.type) {
    // These attribute types are always resident on disk; reject any
    // record claiming otherwise before its bytes get reinterpreted as one.
    case Attr::Type::StandardInformation: {
      attr = MakeResidentOnly<Attr::AttrStdInfo, Resident>(
          ahc, *self, "Standard Information attribute must be resident.\n");
      break;
    }

    // Unresolved, $ATTRIBUTE_LIST is wrapped generically, as the arm below.
    case Attr::Type::AttributeList: {
      attr = resolve_attr_list ? MakeByResidency<Attr::AttrList, Resident>(
                                     ahc, *self, attr_list_chain)
                               : MakeGeneric<Resident>(ahc, *self);
      break;
    }

    case Attr::Type::FileName: {
      attr = MakeResidentOnly<Attr::AttrFileName, Resident>(
          ahc, *self, "File Name attribute must be resident.\n");
      break;
    }

    case Attr::Type::VolumeName: {
      attr = MakeResidentOnly<Attr::AttrVolName, Resident>(
          ahc, *self, "Volume Name attribute must be resident.\n");
      break;
    }

    case Attr::Type::VolumeInformation: {
      attr = MakeResidentOnly<Attr::AttrVolInfo, Resident>(
          ahc, *self, "Volume Information attribute must be resident.\n");
      break;
    }

    case Attr::Type::Data: {
      attr = MakeByResidency<Attr::AttrData, Resident>(ahc, *self);
      break;
    }

    case Attr::Type::IndexRoot: {
      attr = MakeResidentOnly<Attr::AttrIndexRoot, Resident>(
          ahc, *self, "Index Root attribute must be resident.\n");
      break;
    }

    case Attr::Type::IndexAllocation: {
      attr = MakeIndexAlloc(ahc, *self);
      break;
    }

    // Resident Bitmap may exist in a directory's FileRecord
    // or in $MFT for a very small volume in theory
    case Attr::Type::Bitmap: {
      attr = MakeByResidency<Attr::AttrBitmap, Resident>(ahc, *self);
      break;
    }

    // Unhandled Attributes
    default:
      unhandled = true;
      [[fallthrough]];
    // $EFS, the only one this library reads, is read through the generic
    // wrappers. Any other logged utility stream is not needed, but not
    // worth a warning either.
    case Attr::Type::LoggedUtilityStream: {
      attr = MakeGeneric<Resident>(ahc, *self);
      break;
    }
  }
  return attr;
}

// Parse a single Attribute
// Return False on error
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseAttr(
    const Attr::HeaderCommon& ahc,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  const DWORD attr_index = Attr::AttrIndex(ahc.type);
  if (attr_index >= Attr::attr_nums) {
    Log::Warn("Invalid Attribute Type: 0x{:04X}", static_cast<DWORD>(ahc.type));
    return false;
  }

  bool discard = false;
  UserCallBack(attr_index, ahc, discard);

  if (discard) {
    Log::Debug("User Callback has processed this Attribute: 0x{:04X}",
               static_cast<DWORD>(ahc.type));
    return true;
  }

  bool unhandled = false;

  std::unique_ptr<Attr::AttrBase<S>> attr;
  try {
    if constexpr (S == Cache::Strategy::NoCache) {
      attr =
          AllocAttr<Attr::AttrResidentNoCache>(ahc, unhandled, attr_list_chain);
    } else {
      attr = AllocAttr<Attr::AttrResidentFullCache>(ahc, unhandled,
                                                    attr_list_chain);
    }
  } catch (const std::exception& e) {
    // gsl::narrow(), reachable through AllocAttr(), can throw a
    // gsl::narrowing_error, which is not a std::runtime_error.
    Log::Error("Attribute Parse error: 0x{:04X}", static_cast<DWORD>(ahc.type));
    Log::Exception(e);
    return false;
  }

  if (unhandled) {
    Log::Warn("Unhandled attribute: 0x{:04X}", static_cast<DWORD>(ahc.type));
  }
  // attrIndex < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  attr_list[attr_index].push_back(std::move(attr));
  return true;
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

// attrListChain carries one $ATTRIBUTE_LIST resolution's already-visited
// (record, attribute type) pairs into this record's own attribute parse,
// instead of starting a fresh chain.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::ParseAttrs(
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  assert(file_record);

  // Clear previous data
  ClearAttrs();

  const bool recover = volume->GetOptions().recover_errors;

  // Visit all attributes
  const Attr::HeaderCommon* first = FirstAttrToWalk();
  if (first == nullptr) {
    return false;
  }

  const WalkEnd end = WalkAttrHeaders(*first, attr_list_chain);
  if (end == WalkEnd::RanOut) {
    Log::Recoverable(recover,
                     "Attribute walk ended without a terminating end marker.");
  }

  // The walk aborted, or ran out before its end marker. Strict drops everything
  // parsed so far. Recovering keeps it, and that partial result must still be
  // usable: its VCN continuations merged, its encrypted streams given their
  // context.
  if (!recover && end != WalkEnd::EndMarker) {
    ClearAttrs();
    return false;
  }

  MergeAttributeContinuations();

  if (end == WalkEnd::Aborted) {
    // Only a strict parse can fail here.
    static_cast<void>(AttachEfsContext(*self));
    return false;
  }

  if (!AttachEfsContext(*self)) {
    ClearAttrs();
    return false;
  }
  return true;
}

// Returns the header of the first attribute to walk, or nullptr when this
// record must not be parsed: it is freed and not opted in, or has no header.
template <Cache::Strategy S>
const Attr::HeaderCommon* Io::FileRecord<S>::Impl::FirstAttrToWalk() {
  // A freed record still parsed its header (IsDeleted() works), but not its
  // attributes: this record exposes no content unless include_deleted opted
  // in, or this is one of the volume's own metadata reads.
  if (!bypass_deleted_gate && self->IsDeleted() &&
      !volume->GetOptions().include_deleted) {
    Log::Debug("ParseAttrs() skipped: file record {} is deleted",
               file_reference.value_or(0));
    return nullptr;
  }
  return file_record->HeaderCommon();
}

// Walks the attribute headers from first to the end marker, parsing each
// wanted attribute. Stops early at a malformed header, or where VisitAttr
// fails.
template <Cache::Strategy S>
Io::WalkEnd Io::FileRecord<S>::Impl::WalkAttrHeaders(
    const Attr::HeaderCommon& first,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // guard if data exceeds file_record_size_ bounds
  DWORD data_ptr = file_record->GetData()->offset_of_attr;

  // An attribute's position comes from the disk, so it need not be aligned
  // for Attr::HeaderCommon. The walk reads each header through a copy.
  std::span<const BYTE> cur(reinterpret_cast<const BYTE*>(&first),
                            volume->GetFileRecordSize() - data_ptr);

  while (true) {
    Attr::HeaderCommon head{};
    const AttrWalk step =
        ReadAttrHeader(cur, data_ptr, volume->GetFileRecordSize(), head);
    if (step == AttrWalk::EndMarker) {
      return WalkEnd::EndMarker;
    }
    if (step == AttrWalk::RanOut) {
      return WalkEnd::RanOut;
    }
    if (step == AttrWalk::Abort || !VisitAttr(cur, head, attr_list_chain)) {
      return WalkEnd::Aborted;
    }

    data_ptr += head.total_size;
    cur = cur.subspan(head.total_size);  // next attribute
  }
}

// Parses the attribute at the front of cur, if its type is wanted. Returns
// false when it must end the walk.
template <Cache::Strategy S>
bool Io::FileRecord<S>::Impl::VisitAttr(
    std::span<const BYTE> cur, const Attr::HeaderCommon& head,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // True only when the type is a real attribute slot and the caller's
  // mask requests that slot.
  if (!Attr::IsValidAttrType(head.type) ||
      !static_cast<bool>(Attr::AttrMask(head.type) & attr_mask)) {
    return true;
  }

  // Mirrors AttrBase::GetAttrName()'s own bounds check, ahead of
  // constructing the attribute: strict rejects it outright instead of
  // parsing it and letting a later GetAttrName() call find the same
  // defect.
  const bool name_exceeds_bounds =
      head.name_length != 0 &&
      static_cast<ULONGLONG>(head.name_offset) +
              (static_cast<ULONGLONG>(head.name_length) * sizeof(WCHAR)) >
          head.total_size;
  if (name_exceeds_bounds) {
    const bool recover = volume->GetOptions().recover_errors;
    Log::Recoverable(recover, "Attribute name exceeds attribute bounds.");
    if (!recover) {
      return false;
    }
  }

  return ParseAttr(AlignedAttrHeader(cur), attr_list_chain);
}

namespace {

// Every non-resident instance's index into attrs, grouped by stream name: two
// differently named streams of the same type (eg. two ADS) must never merge
// into each other.
template <Cache::Strategy S>
std::unordered_map<std::wstring, std::vector<size_t>> GroupNonResidentByName(
    const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs) {
  std::unordered_map<std::wstring, std::vector<size_t>> by_name;
  for (size_t i = 0; i < attrs.size(); ++i) {
    // i < attrs.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& attr = attrs[i];
    if (attr->IsNonResident()) {
      std::wstring key;
      if (!attr->IsUnNamed()) {
        key = attr->GetAttrName();
      }
      by_name[key].push_back(i);
    }
  }
  return by_name;
}

}  // namespace

// Merges the VCN-split instances at indices, all of one stream, into the first
// one. Appends the indices of the absorbed instances to toErase. A gap, an
// overlap, or a chain that doesn't start at VCN 0 means a damaged or
// unsupported layout: every instance is then left exactly as parsed instead of
// splicing a wrong or partial result together.
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::MergeStreamChain(
    std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs,
    std::vector<size_t>& indices, std::vector<size_t>& to_erase) {
  // indices only holds indices below attrs.size().
  const auto non_resident = [&attrs](size_t idx) -> Attr::AttrNonResident<S>& {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return static_cast<Attr::AttrNonResident<S>&>(*attrs[idx]);
  };

  std::ranges::sort(
      indices, {}, [&](size_t idx) { return non_resident(idx).GetStartVcn(); });

  ULONGLONG expected_start_vcn = 0;
  bool contiguous = true;
  for (const size_t idx : indices) {
    const auto& instance = non_resident(idx);
    if (instance.GetStartVcn() != expected_start_vcn) {
      contiguous = false;
      break;
    }
    expected_start_vcn = instance.GetLastVcn() + 1;
  }
  if (!contiguous) {
    Log::Warn(
        "Attribute continuation VCNs are not contiguous from 0; leaving "
        "{} instance(s) unmerged",
        indices.size());
    return;
  }

  auto& keeper = non_resident(indices.front());
  for (size_t k = 1; k < indices.size(); ++k) {
    // k < indices.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t idx = indices[k];
    keeper.AppendRuns(non_resident(idx));
    to_erase.push_back(idx);
  }
}

// Splices a non-resident attribute's own VCN-split instances (already all in
// attr_list_ by now) back into one, so getAttr()/FindStream() see exactly
// one complete attribute per stream instead of several partial ones.
template <Cache::Strategy S>
void Io::FileRecord<S>::Impl::MergeAttributeContinuations() {
  for (std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs : attr_list) {
    if (attrs.size() < 2) {
      continue;
    }

    std::vector<size_t> to_erase;
    for (auto& [name, indices] : GroupNonResidentByName<S>(attrs)) {
      if (indices.size() >= 2) {
        MergeStreamChain(attrs, indices, to_erase);
      }
    }

    std::ranges::sort(to_erase);
    for (const auto index : to_erase | std::views::reverse) {
      attrs.erase(attrs.begin() + gsl::narrow<ptrdiff_t>(index));
    }
  }
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
    const Filename* filename = nullptr;
    if constexpr (S == Cache::Strategy::NoCache) {
      filename = static_cast<const Attr::AttrFileName<
          Attr::AttrResidentNoCache, Cache::Strategy::NoCache>*>(fn.get());
    } else {
      filename = static_cast<const Attr::AttrFileName<
          Attr::AttrResidentFullCache, Cache::Strategy::FullCache>*>(fn.get());
    }

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
  if constexpr (S == Cache::Strategy::NoCache) {
    return reinterpret_cast<const Attr::AttrFileName<
        Attr::AttrResidentNoCache, Cache::Strategy::NoCache>*>(
               vec.front().get())
        ->GetFileSize();
  } else if constexpr (S == Cache::Strategy::FullCache) {
    return reinterpret_cast<const Attr::AttrFileName<
        Attr::AttrResidentFullCache, Cache::Strategy::FullCache>*>(
               vec.front().get())
        ->GetFileSize();
  }
  return 0;
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
  if (const auto* std_info = StdInfoOf(*this); std_info != nullptr) {
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
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsReadOnly();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsHidden() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsHidden();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSystem() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsSystem();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsArchive() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsArchive();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsDevice() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsDevice();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsNormal() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsNormal();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsTemporary() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsTemporary();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsCompressed() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsCompressed();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsOffline() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsOffline();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsNotContentIndexed() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsNotContentIndexed();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsEncrypted() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsEncrypted();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsSparse() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsSparse();
}

template <Cache::Strategy S>
bool Io::FileRecord<S>::IsReparsePoint() const noexcept {
  const auto* std_info = StdInfoOf(*this);
  return std_info != nullptr && std_info->IsReparsePoint();
}

template class FileRecord<Cache::Strategy::NoCache>;
template class FileRecord<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser::Io
