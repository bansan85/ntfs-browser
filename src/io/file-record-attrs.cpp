#include "io/file-record-attrs.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
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
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/bitmap.h"  // IWYU pragma: keep
#include "attr/data.h"    // IWYU pragma: keep
#include "attr/file-name.h"
#include "attr/index-alloc.h"  // IWYU pragma: keep
#include "attr/index-root.h"
#include "attr/list.h"          // IWYU pragma: keep
#include "attr/non-resident.h"  // IWYU pragma: keep
#include "attr/resident.h"
#include "attr/std-info.h"
#include "attr/vol-info.h"  // IWYU pragma: keep
#include "attr/vol-name.h"  // IWYU pragma: keep
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "io/file-record-efs.h"
#include "io/file-record-impl.h"
#include "log/ntfs-common.h"

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
template <template <typename, Cache::Strategy> class Wrapper, Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>>
    MakeResidentOnly(const Attr::HeaderCommon& ahc, FileRecord<S>& record,
                     const char* message) {
  RequireResident(ahc, message);
  return MakeAttr<Wrapper, ResidentOf<S>>(ahc, record);
}

// Wraps the non-resident form of a type when the header is non-resident,
// else its resident form. Args are passed on after the record.
template <template <typename, Cache::Strategy> class Wrapper, Cache::Strategy S,
          typename... Args>
std::unique_ptr<Attr::AttrBase<S>>
    MakeByResidency(const Attr::HeaderCommon& ahc, FileRecord<S>& record,
                    Args&&... args) {
  if (ahc.non_resident != 0) {
    return MakeAttr<Wrapper, Attr::AttrNonResident<S>>(
        ahc, record, std::forward<Args>(args)...);
  }
  return MakeAttr<Wrapper, ResidentOf<S>>(ahc, record,
                                          std::forward<Args>(args)...);
}

// Wraps a type with no class of its own, as its non-resident or resident base.
template <Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>> MakeGeneric(const Attr::HeaderCommon& ahc,
                                               FileRecord<S>& record) {
  if (ahc.non_resident != 0) {
    return std::make_unique<Attr::AttrNonResident<S>>(ahc, record);
  }
  return std::make_unique<ResidentOf<S>>(ahc, record);
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

template <Cache::Strategy S>
Io::AttrParser<S>::AttrParser(typename FileRecord<S>::Impl& impl) noexcept
    : impl_(impl) {}

template <Cache::Strategy S>
bool Io::AttrParser<S>::Parse(std::unordered_set<ULONGLONG>& attr_list_chain) {
  assert(impl_.file_record);

  // Clear previous data
  impl_.ClearAttrs();

  const bool recover = impl_.volume->GetOptions().recover_errors;

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
    impl_.ClearAttrs();
    return false;
  }

  MergeAttributeContinuations();

  if (end == WalkEnd::Aborted) {
    // Only a strict parse can fail here.
    static_cast<void>(AttachEfsContext(*impl_.self));
    return false;
  }

  if (!AttachEfsContext(*impl_.self)) {
    impl_.ClearAttrs();
    return false;
  }
  return true;
}

// Returns the header of the first attribute to walk, or nullptr when this
// record must not be parsed: it is freed and not opted in, or has no header.
template <Cache::Strategy S>
const Attr::HeaderCommon* Io::AttrParser<S>::FirstAttrToWalk() {
  // A freed record still parsed its header (IsDeleted() works), but not its
  // attributes: this record exposes no content unless include_deleted opted
  // in, or this is one of the volume's own metadata reads.
  if (!impl_.bypass_deleted_gate && impl_.self->IsDeleted() &&
      !impl_.volume->GetOptions().include_deleted) {
    Log::Debug("ParseAttrs() skipped: file record {} is deleted",
               impl_.file_reference.value_or(0));
    return nullptr;
  }
  return impl_.file_record->HeaderCommon();
}

// Walks the attribute headers from first to the end marker, parsing each
// wanted attribute. Stops early at a malformed header, or where VisitAttr
// fails.
template <Cache::Strategy S>
WalkEnd Io::AttrParser<S>::WalkAttrHeaders(
    const Attr::HeaderCommon& first,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // guard if data exceeds file_record_size_ bounds
  DWORD data_ptr = impl_.file_record->GetData()->offset_of_attr;

  // An attribute's position comes from the disk, so it need not be aligned
  // for Attr::HeaderCommon. The walk reads each header through a copy.
  std::span<const BYTE> cur(reinterpret_cast<const BYTE*>(&first),
                            impl_.volume->GetFileRecordSize() - data_ptr);

  while (true) {
    Attr::HeaderCommon head{};
    const AttrWalk step =
        ReadAttrHeader(cur, data_ptr, impl_.volume->GetFileRecordSize(), head);
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
bool Io::AttrParser<S>::VisitAttr(
    std::span<const BYTE> cur, const Attr::HeaderCommon& head,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // True only when the type is a real attribute slot and the caller's
  // mask requests that slot.
  if (!Attr::IsValidAttrType(head.type) ||
      !static_cast<bool>(Attr::AttrMask(head.type) & impl_.attr_mask)) {
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
    const bool recover = impl_.volume->GetOptions().recover_errors;
    Log::Recoverable(recover, "Attribute name exceeds attribute bounds.");
    if (!recover) {
      return false;
    }
  }

  return ParseAttr(AlignedAttrHeader(cur), attr_list_chain);
}

// Returns the attribute header at the start of `at`, the bytes from there to
// the end of the record: in place when it is aligned for the on-disk structs,
// else in an aligned copy of `at`. The copy keeps every read the attribute
// makes inside the record, as in place.
template <Cache::Strategy S>
const Attr::HeaderCommon&
    Io::AttrParser<S>::AlignedAttrHeader(std::span<const BYTE> bytes) {
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) %
          alignof(Data::HeaderNonResident) ==
      0) {
    return *reinterpret_cast<const Attr::HeaderCommon*>(bytes.data());
  }

  auto& copy = impl_.realigned_attrs.emplace_back(bytes.size());
  std::memcpy(copy.data(), bytes.data(), bytes.size());
  return *reinterpret_cast<const Attr::HeaderCommon*>(copy.data());
}

// Wraps one raw attribute in the class that matches its type. unhandled is
// set for a type this library has no wrapper for.
template <Cache::Strategy S>
std::unique_ptr<Attr::AttrBase<S>> Io::AttrParser<S>::AllocAttr(
    const Attr::HeaderCommon& ahc, bool& unhandled,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  FileRecord<S>& record = *impl_.self;
  switch (ahc.type) {
    // These attribute types are always resident on disk; reject any
    // record claiming otherwise before its bytes get reinterpreted as one.
    case Attr::Type::StandardInformation:
      return MakeResidentOnly<Attr::AttrStdInfo>(
          ahc, record, "Standard Information attribute must be resident.\n");

    // Unresolved, $ATTRIBUTE_LIST is wrapped generically, as the arm below.
    case Attr::Type::AttributeList:
      return impl_.resolve_attr_list
                 ? MakeByResidency<Attr::AttrList>(ahc, record, attr_list_chain)
                 : MakeGeneric(ahc, record);

    case Attr::Type::FileName:
      return MakeResidentOnly<Attr::AttrFileName>(
          ahc, record, "File Name attribute must be resident.\n");

    case Attr::Type::VolumeName:
      return MakeResidentOnly<Attr::AttrVolName>(
          ahc, record, "Volume Name attribute must be resident.\n");

    case Attr::Type::VolumeInformation:
      return MakeResidentOnly<Attr::AttrVolInfo>(
          ahc, record, "Volume Information attribute must be resident.\n");

    case Attr::Type::Data:
      return MakeByResidency<Attr::AttrData>(ahc, record);

    case Attr::Type::IndexRoot:
      return MakeResidentOnly<Attr::AttrIndexRoot>(
          ahc, record, "Index Root attribute must be resident.\n");

    case Attr::Type::IndexAllocation:
      return MakeIndexAlloc(ahc, record);

    // Resident Bitmap may exist in a directory's FileRecord
    // or in $MFT for a very small volume in theory
    case Attr::Type::Bitmap:
      return MakeByResidency<Attr::AttrBitmap>(ahc, record);

    // Unhandled Attributes
    default:
      unhandled = true;
      [[fallthrough]];
    // $EFS, the only one this library reads, is read through the generic
    // wrappers. Any other logged utility stream is not needed, but not
    // worth a warning either.
    case Attr::Type::LoggedUtilityStream:
      return MakeGeneric(ahc, record);
  }
}

// Parse a single Attribute
// Return False on error
template <Cache::Strategy S>
bool Io::AttrParser<S>::ParseAttr(
    const Attr::HeaderCommon& ahc,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // VisitAttr() admits only a type IsValidAttrType() accepts. Every such type
  // has an index below attr_nums (see slot.h), so the callback table is safe.
  assert(Attr::IsValidAttrType(ahc.type));
  const DWORD attr_index = Attr::AttrIndex(ahc.type);

  bool discard = false;
  impl_.UserCallBack(attr_index, ahc, discard);

  if (discard) {
    Log::Debug("User Callback has processed this Attribute: 0x{:04X}",
               static_cast<DWORD>(ahc.type));
    return true;
  }

  bool unhandled = false;

  std::unique_ptr<Attr::AttrBase<S>> attr;
  try {
    attr = AllocAttr(ahc, unhandled, attr_list_chain);
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
  // attr_index < attr_nums: see the assert at the top.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_.attr_list[attr_index].push_back(std::move(attr));
  return true;
}

// Merges the VCN-split instances at indices, all of one stream, into the first
// one. Appends the indices of the absorbed instances to toErase. A gap, an
// overlap, or a chain that doesn't start at VCN 0 means a damaged or
// unsupported layout: every instance is then left exactly as parsed instead of
// splicing a wrong or partial result together.
template <Cache::Strategy S>
void Io::AttrParser<S>::MergeStreamChain(
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
void Io::AttrParser<S>::MergeAttributeContinuations() {
  for (std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs :
       impl_.attr_list) {
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

template class AttrParser<Cache::Strategy::NoCache>;
template class AttrParser<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser::Io
