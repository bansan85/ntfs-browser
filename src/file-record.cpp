#include "flag/file-record.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "attr-bitmap.h"  // IWYU pragma: keep
#include "attr-data.h"    // IWYU pragma: keep
#include "attr-file-name.h"
#include "attr-index-alloc.h"  // IWYU pragma: keep
#include "attr-index-root.h"
#include "attr-list.h"          // IWYU pragma: keep
#include "attr-non-resident.h"  // IWYU pragma: keep
#include "attr-resident.h"
#include "attr-slot.h"
#include "attr-std-info.h"
#include "attr-vol-info.h"  // IWYU pragma: keep
#include "attr-vol-name.h"  // IWYU pragma: keep
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "data/file-record-header.h"
#include "efs/efs-context.h"
#include "efs/efs-stream.h"
#include "file-record-impl.h"
#include "index-block.h"
#include "mft-file-reference.h"
#include "ntfs-common.h"
#include "ntfs-volume-impl.h"
#include "upcase.h"
#include "utf.h"

namespace NtfsBrowser {

template <Strategy S>
class AttrBase;

namespace {

// Chosen well above any real NTFS directory's B+ tree depth, but low enough
// to unwind long before a forged chain overflows the stack.
constexpr size_t max_index_block_depth = 64;

// Caps the orphan-block recovery scan: an attacker-controlled declared block
// count must not drive an unbounded number of ParseIndexBlock() calls. 65536
// blocks is already far past any real directory's index, so this only ever
// binds on a forged/damaged $INDEX_ALLOCATION.
constexpr size_t max_orphan_scan_blocks = 65536;

// Rejects an attribute of a type that is always resident on disk but claims
// to be non-resident, before its bytes get reinterpreted as a resident one.
void RequireResident(const AttrHeaderCommon& ahc, const char* message) {
  if (ahc.non_resident != 0) {
    throw std::runtime_error(message);
  }
}

// How reading the next attribute header of a record's walk ended.
enum class AttrWalk : BYTE {
  Next,       // head holds a header whose attribute fits in the record.
  EndMarker,  // The terminating AttrType::ALL marker was reached.
  RanOut,     // The record ended without a marker, or an attribute overran it.
  Abort       // The attribute is malformed.
};

// Reads the attribute header at the front of cur, and checks that it and its
// attribute fit in a record of recordSize bytes, dataPtr bytes into it.
AttrWalk ReadAttrHeader(std::span<const BYTE> cur, DWORD data_ptr,
                        DWORD record_size, AttrHeaderCommon& head) {
  // The on-disk end-of-attributes marker is a single AttrType::ALL value
  // (4 bytes): check for it as soon as that much room remains, rather
  // than requiring a full attribute header to fit first.
  if (static_cast<ULONGLONG>(data_ptr) + sizeof(AttrType) > record_size) {
    return AttrWalk::RanOut;
  }
  AttrType type{};
  std::memcpy(&type, cur.data(), sizeof(type));
  if (type == AttrType::All) {
    return AttrWalk::EndMarker;
  }
  // From here on, the walk needs the whole header, and the whole
  // attribute, to fit.
  if (static_cast<ULONGLONG>(data_ptr) + sizeof(AttrHeaderCommon) >
      record_size) {
    return AttrWalk::RanOut;
  }
  std::memcpy(&head, cur.data(), sizeof(head));
  if (static_cast<ULONGLONG>(data_ptr) + head.total_size > record_size) {
    return AttrWalk::RanOut;
  }

  const DWORD min_total_size =
      head.non_resident != 0 ? Attr::header_non_resident_base_size
                             : static_cast<DWORD>(sizeof(Attr::HeaderResident));
  if (head.total_size < min_total_size) {
    LogWarn("Attribute total_size too small for its header.");
    return AttrWalk::Abort;
  }

  if (head.non_resident != 0) {
    Attr::HeaderNonResident non_resident{};
    std::memcpy(&non_resident, cur.data(), sizeof(non_resident));
    if (Attr::HasCompressedSizeField(non_resident) &&
        head.total_size < min_total_size + Attr::compressed_size_field_size) {
      LogWarn(
          "Compressed attribute total_size too small for its compressed "
          "size field.");
      return AttrWalk::Abort;
    }
  }
  return AttrWalk::Next;
}

}  // namespace

template <Strategy S>
FileRecord<S>::Impl::Impl(FileRecord<S>& self,
                          const NtfsVolume<S>& volume) noexcept
    : self(&self), volume(&volume) {}

template <Strategy S>
FileRecord<S>::FileRecord(const NtfsVolume<S>& volume)
    : impl_(std::make_unique<Impl>(*this, volume)) {}

template <Strategy S>
FileRecord<S>::FileRecord(FileRecord&& other) noexcept
    : impl_(std::move(other.impl_)) {
  impl_->self = this;
}

template <Strategy S>
FileRecord<S>::~FileRecord() = default;

template <Strategy S>
const NtfsVolume<S>& FileRecord<S>::GetVolume() const noexcept {
  return *impl_->volume;
}

// Drops every parsed attribute, then the records and copies they point into.
template <Strategy S>
void FileRecord<S>::Impl::ClearAttrs() noexcept {
  for (std::vector<std::unique_ptr<AttrBase<S>>>& arr : attr_list) {
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
template <Strategy S>
const AttrHeaderCommon&
    FileRecord<S>::Impl::AlignedAttrHeader(std::span<const BYTE> bytes) {
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) %
          alignof(Attr::HeaderNonResident) ==
      0) {
    return *reinterpret_cast<const AttrHeaderCommon*>(bytes.data());
  }

  auto& copy = realigned_attrs.emplace_back(bytes.size());
  std::memcpy(copy.data(), bytes.data(), bytes.size());
  return *reinterpret_cast<const AttrHeaderCommon*>(copy.data());
}

// Call user defined Callback routines for an attribute
template <Strategy S>
void FileRecord<S>::Impl::UserCallBack(DWORD att_type,
                                       const AttrHeaderCommon& ahc,
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
template <Strategy S>
template <typename Resident>
std::unique_ptr<AttrBase<S>> FileRecord<S>::Impl::AllocAttr(
    const AttrHeaderCommon& ahc, bool& unhandled,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  switch (ahc.type) {
    // These attribute types are always resident on disk; reject any
    // record claiming otherwise before its bytes get reinterpreted as one.
    case AttrType::StandardInformation:
      RequireResident(ahc,
                      "Standard Information attribute must be resident.\n");
      return std::make_unique<AttrStdInfo<Resident, S>>(ahc, *self);

    case AttrType::AttributeList:
      if (!resolve_attr_list) {
        if (ahc.non_resident != 0) {
          return std::make_unique<AttrNonResident<S>>(ahc, *self);
        }
        return std::make_unique<Resident>(ahc, *self);
      }
      if (ahc.non_resident != 0) {
        return std::make_unique<AttrList<AttrNonResident<S>, S>>(
            ahc, *self, attr_list_chain);
      }
      return std::make_unique<AttrList<Resident, S>>(ahc, *self,
                                                     attr_list_chain);

    case AttrType::FileName:
      RequireResident(ahc, "File Name attribute must be resident.\n");
      return std::make_unique<AttrFileName<Resident, S>>(ahc, *self);

    case AttrType::VolumeName:
      RequireResident(ahc, "Volume Name attribute must be resident.\n");
      return std::make_unique<AttrVolName<Resident, S>>(ahc, *self);

    case AttrType::VolumeInformation:
      RequireResident(ahc, "Volume Information attribute must be resident.\n");
      return std::make_unique<AttrVolInfo<Resident, S>>(ahc, *self);

    case AttrType::Data:
      if (ahc.non_resident != 0) {
        return std::make_unique<AttrData<AttrNonResident<S>, S>>(ahc, *self);
      }
      return std::make_unique<AttrData<Resident, S>>(ahc, *self);

    case AttrType::IndexRoot:
      RequireResident(ahc, "Index Root attribute must be resident.\n");
      return std::make_unique<AttrIndexRoot<Resident, S>>(ahc, *self);

    // INDEX_ALLOCATION is always non-resident on disk; reject a record
    // claiming otherwise before its bytes get reinterpreted as one.
    case AttrType::IndexAllocation:
      if (ahc.non_resident == 0) {
        throw std::runtime_error(
            "Index Allocation attribute must be non-resident.\n");
      }
      return std::make_unique<AttrIndexAlloc<S>>(ahc, *self);

    case AttrType::Bitmap:
      if (ahc.non_resident != 0) {
        return std::make_unique<AttrBitmap<AttrNonResident<S>, S>>(ahc, *self);
      }
      // Resident Bitmap may exist in a directory's FileRecord
      // or in $MFT for a very small volume in theory
      return std::make_unique<AttrBitmap<Resident, S>>(ahc, *self);

    // $EFS, the only one this library reads, is read through the generic
    // wrappers. Any other logged utility stream is not needed, but not
    // worth a warning either.
    case AttrType::LoggedUtilityStream:
      if (ahc.non_resident != 0) {
        return std::make_unique<AttrNonResident<S>>(ahc, *self);
      }
      return std::make_unique<Resident>(ahc, *self);

    // Unhandled Attributes
    default:
      unhandled = true;
      if (ahc.non_resident != 0) {
        return std::make_unique<AttrNonResident<S>>(ahc, *self);
      }
      return std::make_unique<Resident>(ahc, *self);
  }
}

// Parse a single Attribute
// Return False on error
template <Strategy S>
bool FileRecord<S>::Impl::ParseAttr(
    const AttrHeaderCommon& ahc,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  const DWORD attr_index = AttrIndex(ahc.type);
  if (attr_index >= attr_nums) {
    LogWarn("Invalid Attribute Type: 0x{:04X}", static_cast<DWORD>(ahc.type));
    return false;
  }

  bool discard = false;
  UserCallBack(attr_index, ahc, discard);

  if (discard) {
    LogDebug("User Callback has processed this Attribute: 0x{:04X}",
             static_cast<DWORD>(ahc.type));
    return true;
  }

  bool unhandled = false;

  std::unique_ptr<AttrBase<S>> attr;
  try {
    if constexpr (S == Strategy::NoCache) {
      attr = AllocAttr<AttrResidentNoCache>(ahc, unhandled, attr_list_chain);
    } else {
      attr = AllocAttr<AttrResidentFullCache>(ahc, unhandled, attr_list_chain);
    }
  } catch (const std::exception& e) {
    // gsl::narrow(), reachable through AllocAttr(), can throw a
    // gsl::narrowing_error, which is not a std::runtime_error.
    LogError("Attribute Parse error: 0x{:04X}", static_cast<DWORD>(ahc.type));
    LogException(e);
    return false;
  }

  if (unhandled) {
    LogWarn("Unhandled attribute: 0x{:04X}", static_cast<DWORD>(ahc.type));
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
template <Strategy S>
std::unique_ptr<FileRecordHeaderImpl<S>>
    FileRecord<S>::Impl::ReadFileRecord(ULONGLONG file_ref) {
  if (record_buffer.size() != volume->GetFileRecordSize()) {
    record_buffer.resize(volume->GetFileRecordSize());
  }

  if (file_ref < static_cast<ULONGLONG>(Enum::MftIdx::User) ||
      volume->impl_->mft_data == nullptr) {
    // Take as continuous disk allocation
    LARGE_INTEGER fr_addr{};
    try {
      fr_addr.QuadPart = gsl::narrow<LONGLONG>(
          volume->GetMFTAddr() + volume->GetFileRecordSize() * file_ref);
    } catch (const std::exception& e) {
      // fileRef is attacker-controlled and unbounded, so this sum can
      // still overflow a LONGLONG even with mft_addr_ validated.
      LogException(e);
      return {};
    }

    if (!volume->ReadInto(fr_addr, record_buffer)) {
      return {};
    }

    try {
      return std::make_unique<FileRecordHeaderImpl<S>>(record_buffer);
    } catch (const std::exception& e) {
      LogException(e);
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
    return std::make_unique<FileRecordHeaderImpl<S>>(record_buffer);
  } catch (const std::exception& e) {
    // Reachable through the same constructor call as the direct-allocation
    // path above.
    LogException(e);
    return {};
  }
}

// Read File Record, verify and patch the US (update sequence)
template <Strategy S>
bool FileRecord<S>::ParseFileRecord(ULONGLONG file_ref) {
  // Clear previous data
  impl_->ClearAttrs();
  if (impl_->file_record) {
    impl_->file_record.reset();
  }

  std::unique_ptr<FileRecordHeaderImpl<S>> header =
      impl_->ReadFileRecord(file_ref);
  if (!header) {
    LogError("Cannot read file record {}", file_ref);

    impl_->file_reference = {};

    return false;
  }

  impl_->file_reference = file_ref;

  // Debug, not warning: a slot NTFS never used has no magic, so an MFT scan
  // meets this on every such slot. A caller gets false either way.
  if (header->GetData()->magic != file_record_magic) {
    LogDebug("Invalid file record");
    return false;
  }

  if (!header->PatchUS()) {
    LogWarn("Update Sequence Number error");
    return false;
  }

  LogDebug("File Record {} Found", file_ref);
  impl_->file_record = std::move(header);

  return true;
}

// Visit IndexBlocks recursivly to find a specific Filename
template <Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<IndexEntry> FileRecord<S>::Impl::VisitIndexBlock(
    ULONGLONG vcn, std::wstring_view file_name,
    std::unordered_set<ULONGLONG>& visited_vcns, size_t depth) const {
  if (depth >= max_index_block_depth) {
    LogWarn("VisitIndexBlock() aborting: recursion depth limit exceeded");
    return {};
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visited_vcns.insert(vcn).second) {
    return {};
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      self->GetAttr(AttrType::IndexAllocation);
  if (vec.empty()) {
    return {};
  }

  IndexBlock index_block;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, index_block)) {
    return {};
  }

  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.HasName()) {
      // Compare name
      const int comparison =
          index_entry.Compare(file_name, volume->impl_->GetUpCaseTable());
      if (comparison == 0) {
        // Must be a copy: the view dies with index_block, the IndexEntry
        // keeps its own bytes.
        LogDebug("VisitIndexBlock() found entry in sub-node");
        return IndexEntry(index_entry);
      }
      if (comparison < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (!index_entry.IsSubNodePtr()) {
          return {};  // not found
        }
        // Search in SubNode (IndexBlock), recursive call
        std::optional<IndexEntry> retval = VisitIndexBlock(
            index_entry.GetSubNodeVCN(), file_name, visited_vcns, depth + 1);
        if (retval) {
          return retval;
        }
      }
      // Just step forward if fileName is bigger than IndexEntry
    } else if (index_entry.IsSubNodePtr()) {
      // Search in SubNode (IndexBlock), recursive call
      std::optional<IndexEntry> retval = VisitIndexBlock(
          index_entry.GetSubNodeVCN(), file_name, visited_vcns, depth + 1);
      if (retval) {
        return retval;
      }
    }
  }

  return {};
}

// Traverse SubNode recursivly in ascending order
// Call user defined callback routine once found an subentry
// visitedVcns guards against a malformed/malicious B+ tree where a
// subnode VCN is revisited, which would otherwise recurse without bound.
template <Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
void FileRecord<S>::Impl::TraverseSubNode(
    ULONGLONG vcn, const SubentryCallback& se_call_back, void* context,
    std::unordered_set<ULONGLONG>& visited_vcns, size_t depth) const {
  if (depth >= max_index_block_depth) {
    LogWarn("TraverseSubNode() aborting: recursion depth limit exceeded");
    return;
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visited_vcns.insert(vcn).second) {
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      self->GetAttr(AttrType::IndexAllocation);
  if (vec.empty()) {
    return;
  }

  IndexBlock index_block;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, index_block)) {
    return;
  }

  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.IsSubNodePtr()) {
      // recursive call
      TraverseSubNode(index_entry.GetSubNodeVCN(), se_call_back, context,
                      visited_vcns, depth + 1);
    }

    if (index_entry.HasName()) {
      se_call_back(index_entry, context);
    }
  }
}

// Parse all the attributes in a File Record
// And insert them into a link list
template <Strategy S>
bool FileRecord<S>::ParseAttrs() {
  // A fresh chain, unrelated to any previous ParseFileRecord() on this object.
  std::unordered_set<ULONGLONG> attr_list_chain;
  return impl_->ParseAttrs(attr_list_chain);
}

// attrListChain carries one $ATTRIBUTE_LIST resolution's already-visited
// (record, attribute type) pairs into this record's own attribute parse,
// instead of starting a fresh chain.
template <Strategy S>
bool FileRecord<S>::Impl::ParseAttrs(
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  assert(file_record);

  // Clear previous data
  ClearAttrs();

  const bool recover = volume->GetOptions().recover_errors;

  // Ends the walk early with failure. Strict drops everything parsed so far.
  // Recovering keeps it, and that partial result must still be usable: its
  // VCN continuations merged, its encrypted streams given their context.
  const auto abort_walk = [this, recover] {
    if (!recover) {
      ClearAttrs();
      return false;
    }
    MergeAttributeContinuations();
    // Only a strict parse can fail here.
    static_cast<void>(AttachEfsContext());
    return false;
  };

  // A freed record still parsed its header (IsDeleted() works), but not its
  // attributes: this record exposes no content unless include_deleted opted
  // in, or this is one of the volume's own metadata reads.
  if (!bypass_deleted_gate && self->IsDeleted() &&
      !volume->GetOptions().include_deleted) {
    LogDebug("ParseAttrs() skipped: file record {} is deleted",
             file_reference ? *file_reference : 0);
    return false;
  }

  // Visit all attributes

  DWORD data_ptr = 0;  // guard if data exceeds file_record_size_ bounds
  const AttrHeaderCommon* first = file_record->HeaderCommon();

  if (first == nullptr) {
    return false;
  }

  data_ptr += file_record->GetData()->offset_of_attr;
  bool found_end_marker = false;

  // An attribute's position comes from the disk, so it need not be aligned
  // for AttrHeaderCommon. The walk reads each header through a copy.
  std::span<const BYTE> cur(reinterpret_cast<const BYTE*>(first),
                            volume->GetFileRecordSize() - data_ptr);

  while (true) {
    AttrHeaderCommon head{};
    const AttrWalk step =
        ReadAttrHeader(cur, data_ptr, volume->GetFileRecordSize(), head);
    if (step == AttrWalk::EndMarker) {
      found_end_marker = true;
      break;
    }
    if (step == AttrWalk::RanOut) {
      break;
    }
    if (step == AttrWalk::Abort || !VisitAttr(cur, head, attr_list_chain)) {
      return abort_walk();
    }

    data_ptr += head.total_size;
    cur = cur.subspan(head.total_size);  // next attribute
  }

  if (!found_end_marker) {
    LogRecoverable(recover,
                   "Attribute walk ended without a terminating end marker.");
    if (!recover) {
      ClearAttrs();
      return false;
    }
  }

  MergeAttributeContinuations();

  if (!AttachEfsContext()) {
    ClearAttrs();
    return false;
  }
  return true;
}

// Parses the attribute at the front of cur, if its type is wanted. Returns
// false when it must end the walk.
template <Strategy S>
bool FileRecord<S>::Impl::VisitAttr(
    std::span<const BYTE> cur, const AttrHeaderCommon& head,
    std::unordered_set<ULONGLONG>& attr_list_chain) {
  // True only when the type is a real attribute slot and the caller's
  // mask requests that slot.
  if (!IsValidAttrType(head.type) ||
      !static_cast<bool>(AttrMask(head.type) & attr_mask)) {
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
    LogRecoverable(recover, "Attribute name exceeds attribute bounds.");
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
template <Strategy S>
std::unordered_map<std::wstring, std::vector<size_t>> GroupNonResidentByName(
    const std::vector<std::unique_ptr<AttrBase<S>>>& attrs) {
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
template <Strategy S>
void FileRecord<S>::Impl::MergeStreamChain(
    std::vector<std::unique_ptr<AttrBase<S>>>& attrs,
    std::vector<size_t>& indices, std::vector<size_t>& to_erase) {
  // indices only holds indices below attrs.size().
  const auto non_resident = [&attrs](size_t idx) -> AttrNonResident<S>& {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return static_cast<AttrNonResident<S>&>(*attrs[idx]);
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
    LogWarn(
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
template <Strategy S>
void FileRecord<S>::Impl::MergeAttributeContinuations() {
  for (std::vector<std::unique_ptr<AttrBase<S>>>& attrs : attr_list) {
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

// The largest $EFS stream read. It holds a few key entries, a few KiB at
// most. A forged size must not decide how much memory a parse allocates.
constexpr ULONGLONG max_efs_stream_size = 64ULL * 1024;

// Name of the $LOGGED_UTILITY_STREAM that holds the EFS keys.
constexpr std::wstring_view efs_stream_name = L"$EFS";

// Copies the key entries out of this record's $EFS stream. Returns none if the
// stream is absent or malformed: the parse goes on, and the read that needs
// the key fails, with the cause logged.
template <Strategy S>
std::vector<Efs::WrappedFek> FileRecord<S>::Impl::ReadEfsEntries() const {
#if !(defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
      (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)))
  // Neither backend is compiled in: there is no decryptor to feed keys to.
  return {};
#else
  for (const std::unique_ptr<AttrBase<S>>& attr :
       std::get<AttrIndex(AttrType::LoggedUtilityStream)>(attr_list)) {
    if (attr->GetAttrName() != efs_stream_name) {
      continue;
    }

    const ULONGLONG size = attr->GetDataSize();
    if (size > max_efs_stream_size) {
      LogWarn("$EFS stream is too large: {} bytes.", size);
      return {};
    }

    // Owned copy: under NoCache the attribute's bytes are short-lived.
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    const std::optional<ULONGLONG> read = attr->ReadData(0, bytes);
    if (!read || *read != size) {
      LogWarn("Cannot read the $EFS stream.");
      return {};
    }

    return Efs::ParseEfsStream(bytes).value_or(std::vector<Efs::WrappedFek>{});
  }

  return {};
#endif
}

// Gives every encrypted $DATA stream of this record the context that
// decrypts it. Only a non-resident stream is encrypted: EFS never leaves file
// data inside the record. Returns false only when strict and one of the two
// anomalies below is found: the caller then rejects the whole record instead
// of reading the stream undecrypted.
template <Strategy S>
bool FileRecord<S>::Impl::AttachEfsContext() {
  // AttrHeaderCommon::flags bit 0: the on-disk "compressed" flag. Real NTFS
  // never sets it alongside 0x4000 (compression and encryption are mutually
  // exclusive), but a forged record could. Decrypting a compressed stream's
  // bytes before LZNT1 decoding sees them would corrupt them for no gain, so
  // that combination is left undecrypted rather than misprocessed, when
  // recovering. Checked with or without a decryption backend compiled in:
  // the anomaly is in the flags, not in what can decrypt them.
  constexpr WORD attr_flag_compressed = 0x0001;
  const bool recover = volume->GetOptions().recover_errors;

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  std::vector<AttrNonResident<S>*> encrypted;
#endif

  for (const std::unique_ptr<AttrBase<S>>& attr :
       std::get<AttrIndex(AttrType::Data)>(attr_list)) {
    const WORD flags = attr->GetAttrFlags();
    if ((flags & Efs::attr_flag_encrypted) == 0) {
      continue;
    }
    if ((flags & attr_flag_compressed) != 0) {
      LogRecoverable(recover,
                     "A $DATA stream is flagged both compressed and "
                     "encrypted; NTFS never combines them. Reading it "
                     "undecrypted.");
      if (!recover) {
        return false;
      }
      continue;
    }

    auto* non_resident = dynamic_cast<AttrNonResident<S>*>(attr.get());
    if (non_resident == nullptr) {
      LogRecoverable(recover,
                     "A resident $DATA is flagged encrypted. Read as is.");
      if (!recover) {
        return false;
      }
      continue;
    }

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    encrypted.push_back(non_resident);
#endif
  }

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  if (encrypted.empty()) {
    return true;
  }

  // One context for the record: its streams share one FEK, resolved once.
  const auto context = std::make_shared<const Efs::Context>(
      ReadEfsEntries(),
      [&volume = *volume] { return volume.GetEfsKeyProvider(); },
      volume->GetEfsCipherBackend());
  for (AttrNonResident<S>* stream : encrypted) {
    stream->SetEfsContext(context);
  }
#endif
  return true;
}

template <Strategy S>
std::optional<ULONGLONG> FileRecord<S>::GetFileReference() const noexcept {
  return impl_->file_reference;
}

template <Strategy S>
WORD FileRecord<S>::GetSequenceNumber() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->seq_no : 0;
}

template <Strategy S>
ULONGLONG FileRecord<S>::GetBaseRecordReference() const noexcept {
  return impl_->file_record ? impl_->file_record->GetData()->ref_to_base &
                                  mft_record_number_mask
                            : 0;
}

template <Strategy S>
bool FileRecord<S>::IsExtensionRecord() const noexcept {
  return impl_->file_record && impl_->file_record->GetData()->ref_to_base != 0;
}

// Install Attribute raw data CallBack routines for a single File Record
template <Strategy S>
bool FileRecord<S>::InstallAttrRawCB(AttrType attr_type,
                                     AttrRawCallback callback) noexcept {
  const DWORD at_idx = AttrIndex(attr_type);
  if (at_idx >= attr_nums) {
    return false;
  }

  // atIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_->attr_raw_call_back[at_idx] = callback;
  return true;
}

// Clear all Attribute CallBack routines
template <Strategy S>
void FileRecord<S>::ClearAttrRawCB() noexcept {
  for (AttrRawCallback& callback : impl_->attr_raw_call_back) {
    callback = nullptr;
  }
}

// Choose attributes to handle, unwanted attributes will be discarded silently
template <Strategy S>
void FileRecord<S>::SetAttrMask(Mask mask) noexcept {
  // Standard Information and Attribute List is needed always
  impl_->attr_mask = mask | Mask::StandardInformation | Mask::AttributeList;

  // The $EFS stream holds the key of every encrypted $DATA.
  if ((mask & Mask::Data) == Mask::Data) {
    impl_->attr_mask |= Mask::LoggedUtilityStream;
  }
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
template <Strategy S>
void FileRecord<S>::TraverseAttrs(const AttrsCallback<S>& attr_call_back,
                                  void* context) {
  if (!attr_call_back) {
    LogWarn("TraverseAttrs() called with an empty callback");
    return;
  }

  for (size_t i = 0; i < attr_nums; i++) {
    // skip masked attributes
    if (static_cast<bool>(impl_->attr_mask & (static_cast<Mask>(1U << i)))) {
      // i < attr_nums by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      for (const std::unique_ptr<AttrBase<S>>& attr_base :
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
template <Strategy S>
const std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::GetAttr(AttrType attr_type) const noexcept {
  static const std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attr_idx = AttrIndex(attr_type);

  if (attr_idx >= attr_nums) {
    return dummy;
  }

  // attrIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->attr_list[attr_idx];
}

template <Strategy S>
std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::GetAttr(AttrType attr_type) noexcept {
  static std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attr_idx = AttrIndex(attr_type);

  if (attr_idx >= attr_nums) {
    return dummy;
  }

  // attrIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->attr_list[attr_idx];
}

// Get File Name (First Win32 name)
template <Strategy S>
std::wstring_view FileRecord<S>::GetFileName() const {
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<AttrBase<S>>& fn :
       std::get<AttrIndex(AttrType::FileName)>(impl_->attr_list)) {
    const Filename* filename = nullptr;
    filename = reinterpret_cast<
        const AttrFileName<AttrResident<S>, Strategy::NoCache>*>(fn.get());

    if (filename->IsWin32Name() && !filename->GetFilename().empty()) {
      return filename->GetFilename();
    }
  }

  return {};
}

// Get File Size
template <Strategy S>
ULONGLONG FileRecord<S>::GetFileSize() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::FileName)>(impl_->attr_list);
  if (vec.empty()) {
    return 0;
  }
  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrFileName<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->GetFileSize();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrFileName<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->GetFileSize();
  }
  return 0;
}

template <Strategy S>
ULONGLONG FileRecord<S>::GetAllocatedSize() const noexcept {
  const AttrBase<S>* data = FindStream({});
  return data != nullptr ? data->GetAllocatedSize() : 0;
}

// Get File Times
template <Strategy S>
void FileRecord<S>::GetFileTime(FILETIME* write_tm, FILETIME* create_tm,
                                FILETIME* access_tm,
                                FILETIME* change_tm) const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  // Standard Information attribute hold the most updated file time
  if (!vec.empty()) {
    if constexpr (S == Strategy::NoCache) {
      reinterpret_cast<
          const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
          vec.front().get())
          ->GetFileTime(write_tm, create_tm, access_tm, change_tm);
    } else if constexpr (S == Strategy::FullCache) {
      reinterpret_cast<
          const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
          vec.front().get())
          ->GetFileTime(write_tm, create_tm, access_tm, change_tm);
    }
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

// Traverse all sub directories and files contained
// Call user defined callback routine once found an entry
template <Strategy S>
void FileRecord<S>::TraverseSubEntries(const SubentryCallback& se_call_back,
                                       void* context) const {
  assert(se_call_back);

  const bool recover = impl_->volume->GetOptions().recover_errors;

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      GetAttr(AttrType::IndexRoot);
  if (vec.empty()) {
    // No IndexRoot at all to start the normal walk from, but $INDEX_ALLOCATION
    // blocks may still exist and hold every entry.
    if (recover) {
      std::unordered_set<ULONGLONG> visited_vcns;
      impl_->ScanOrphanedIndexBlocks(se_call_back, context, visited_vcns);
    }
    return;
  }

  const std::vector<IndexEntryView>* all_ie = nullptr;

  if constexpr (S == Strategy::NoCache) {
    const auto* index_root = reinterpret_cast<
        const AttrIndexRoot<AttrResidentNoCache, Strategy::NoCache>*>(
        vec.front().get());

    if (!index_root->IsFileName()) {
      return;
    }
    all_ie = index_root;
  } else if constexpr (S == Strategy::FullCache) {
    const auto* index_root = reinterpret_cast<
        const AttrIndexRoot<AttrResidentFullCache, Strategy::FullCache>*>(
        vec.front().get());

    if (!index_root->IsFileName()) {
      return;
    }
    all_ie = index_root;
  } else {
    assert(false);
    return;
  }

  std::unordered_set<ULONGLONG> visited_vcns;

  for (const IndexEntryView& index_entry : *all_ie) {
    // Visit subnode first
    if (index_entry.IsSubNodePtr()) {
      impl_->TraverseSubNode(index_entry.GetSubNodeVCN(), se_call_back, context,
                             visited_vcns, 0);
    }

    if (index_entry.HasName()) {
      se_call_back(index_entry, context);
    }
  }

  if (recover) {
    impl_->ScanOrphanedIndexBlocks(se_call_back, context, visited_vcns);
  }
}

namespace {

// Number of whole index blocks that mappedClusters clusters hold. A range of
// clusters counts in bytes, not in clusters, since a block can be smaller than
// one. The byte count saturates instead of wrapping.
ULONGLONG MappedIndexBlocks(ULONGLONG mapped_clusters, DWORD cluster_size,
                            DWORD index_block_size) noexcept {
  const ULONGLONG mapped_bytes =
      (cluster_size != 0 &&
       mapped_clusters > std::numeric_limits<ULONGLONG>::max() / cluster_size)
          ? std::numeric_limits<ULONGLONG>::max()
          : mapped_clusters * cluster_size;
  return mapped_bytes / index_block_size;
}

// Decides whether an entry found in an orphaned block is reported. It MUST be
// filed under this very directory (selfRef, when known). With a volume given,
// the record it names MUST also still be in use under the same sequence.
template <Strategy S>
bool IsOrphanEntryReportable(const IndexEntryView& entry,
                             std::optional<ULONGLONG> self_ref,
                             WORD self_sequence, bool self_in_use,
                             const NtfsVolume<S>* check_named_record_in) {
  // An orphaned block may hold a stale entry left over from a file
  // already deleted from this directory, or from an earlier directory
  // that used this record - only report one still filed under this very
  // directory. Same rule as MftTree: a freed directory keeps the entries
  // filed under its sequence from before NTFS bumped it.
  if (self_ref && (entry.GetParentReference() != *self_ref ||
                   !IsSameRecordGeneration(entry.GetParentSequenceNumber(),
                                           self_sequence, self_in_use))) {
    return false;
  }
  // With include_deleted off, also drop an entry whose named record is
  // itself freed, or was reused under a different sequence number.
  if (check_named_record_in != nullptr) {
    FileRecord<S> named(*check_named_record_in);
    return named.ParseFileRecord(entry.GetFileReference()) &&
           !named.IsDeleted() &&
           named.GetSequenceNumber() == entry.GetSequenceNumber();
  }
  return true;
}

}  // namespace

// Recovery pass for TraverseSubEntries(): a corrupt $INDEX_ROOT or internal
// node can leave real $INDEX_ALLOCATION blocks with no surviving pointer to
// them. Since every name appears exactly once in the B+ tree, scanning every
// block the normal walk missed finds them without relying on any pointer at
// all - unlike the normal walk, in VCN order rather than collation order.
template <Strategy S>
void FileRecord<S>::Impl::ScanOrphanedIndexBlocks(
    const SubentryCallback& se_call_back, void* context,
    std::unordered_set<ULONGLONG>& visited_vcns) const {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      self->GetAttr(AttrType::IndexAllocation);
  if (vec.empty()) {
    return;
  }

  auto* alloc = static_cast<AttrIndexAlloc<S>*>(vec.front().get());

  // A sub-node VCN (what visitedVcns holds, and what ParseIndexBlock()
  // expects) is in clusters when a block spans a whole cluster or more, but
  // in index_block_size units when a cluster is too big to hold one - the
  // same conversion ParseIndexBlock() itself applies.
  const DWORD index_block_size = volume->GetIndexBlockSize();
  const DWORD cluster_size = volume->GetClusterSize();
  const ULONGLONG clusters_per_block =
      (cluster_size != 0 && index_block_size >= cluster_size)
          ? index_block_size / cluster_size
          : 1;

  // GetDataSize() (declared real_size) can be forged far past what the
  // attribute's own data runs actually map; the mapped VCN range, every merged
  // instance included, bounds the scan to what the run list claims to cover
  // instead, so a forged size alone can't drive tens of thousands of doomed
  // ParseIndexBlock() calls. A range of clusters counts in bytes, not in
  // clusters, since a block can be smaller than one.
  const ULONGLONG mapped_clusters =
      (alloc->GetLastVcn() >= alloc->GetStartVcn()) ? alloc->TotalClusters()
                                                    : 0;
  const ULONGLONG mapped_block_count =
      MappedIndexBlocks(mapped_clusters, cluster_size, index_block_size);

  const ULONGLONG declared_block_count = alloc->GetIndexBlockCount();
  const ULONGLONG block_count = (declared_block_count < mapped_block_count)
                                    ? declared_block_count
                                    : mapped_block_count;
  const ULONGLONG scan_limit = (block_count < max_orphan_scan_blocks)
                                   ? block_count
                                   : max_orphan_scan_blocks;
  if (declared_block_count > max_orphan_scan_blocks ||
      declared_block_count > mapped_block_count) {
    LogInfo(
        "TraverseSubEntries() recovery: orphan scan capped at {} of {} "
        "index blocks",
        scan_limit, declared_block_count);
  }

  const std::optional<ULONGLONG> self_ref = file_reference;
  const WORD self_sequence = self->GetSequenceNumber();
  const bool self_in_use = !self->IsDeleted();
  const bool include_deleted = volume->GetOptions().include_deleted;

  for (ULONGLONG block_index = 0; block_index < scan_limit; block_index++) {
    const ULONGLONG vcn = block_index * clusters_per_block;
    if (!visited_vcns.insert(vcn).second) {
      continue;
    }

    IndexBlock index_block;
    if (!alloc->ParseIndexBlock(vcn, index_block)) {
      continue;
    }

    LogInfo("TraverseSubEntries() recovery: reporting orphaned index block {}",
            vcn);

    for (const IndexEntryView& index_entry : index_block) {
      if (index_entry.HasName() &&
          IsOrphanEntryReportable(index_entry, self_ref, self_sequence,
                                  self_in_use,
                                  include_deleted ? nullptr : volume)) {
        se_call_back(index_entry, context);
      }
    }
  }
}

// Find a specific Filename from InexRoot described B+ tree
template <Strategy S>
std::optional<IndexEntry>
    FileRecord<S>::FindSubEntry(std::wstring_view file_name) const {
  std::optional<IndexEntry> found = impl_->FindSubEntryInOrder(file_name);
  if (found || !impl_->volume->impl_->GetUpCaseTable().IsBuiltIn()) {
    return found;
  }

  // The built-in mapping can disagree with the volume's own collation, and
  // the ordered search then stops at a leaf that is not the end of the
  // name's range. Look at every entry instead.
  LogDebug("FindSubEntry() scans every entry: no $UpCase table");
  TraverseSubEntries(
      [&](const IndexEntryView& index_entry, void*) {
        if (!found &&
            index_entry.Compare(file_name,
                                impl_->volume->impl_->GetUpCaseTable()) == 0) {
          found.emplace(index_entry);
        }
      },
      nullptr);
  return found;
}

// The entries of a file-name $INDEX_ROOT, or null for any other kind of index
// or for a strategy this library does not know.
template <Strategy S>
const std::vector<IndexEntryView>*
    FileRecord<S>::Impl::FileNameIndexRootEntries(const AttrBase<S>& attr) {
  if constexpr (S == Strategy::NoCache) {
    const auto* index_root = reinterpret_cast<
        const AttrIndexRoot<AttrResidentNoCache, Strategy::NoCache>*>(&attr);
    return index_root->IsFileName() ? index_root : nullptr;
  } else if constexpr (S == Strategy::FullCache) {
    const auto* index_root = reinterpret_cast<
        const AttrIndexRoot<AttrResidentFullCache, Strategy::FullCache>*>(
        &attr);
    return index_root->IsFileName() ? index_root : nullptr;
  } else {
    return nullptr;
  }
}

// FindSubEntry()'s walk down the B+ tree, trusting the entries to be sorted
// by the volume's collation order. A name that sorts before a leaf entry is
// reported absent.
template <Strategy S>
std::optional<IndexEntry> FileRecord<S>::Impl::FindSubEntryInOrder(
    std::wstring_view file_name) const {
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      self->GetAttr(AttrType::IndexRoot);
  if (vec.empty()) {
    return {};
  }

  const std::vector<IndexEntryView>* all_ie =
      FileNameIndexRootEntries(*vec.front());
  if (all_ie == nullptr) {
    return {};
  }

  std::unordered_set<ULONGLONG> visited_vcns;
  // Loaded before the walk: reading $UpCase reuses the volume's buffers.
  const UpCaseTable& upcase = volume->impl_->GetUpCaseTable();

  for (const IndexEntryView& index_entry : *all_ie) {
    if (index_entry.HasName()) {
      // Compare name
      const int comparison = index_entry.Compare(file_name, upcase);
      if (comparison == 0) {
        // Must be a copy: the view dies with this FileRecord, the IndexEntry
        // keeps its own bytes.
        LogDebug("FindSubEntry() found entry in Index Root");
        return IndexEntry(index_entry);
      }
      // Just step forward if fileName is bigger than IndexEntry
      if (comparison > 0) {
        continue;
      }
      // fileName is smaller than a leaf entry: not found
      if (!index_entry.IsSubNodePtr()) {
        return {};
      }
    }

    if (index_entry.IsSubNodePtr()) {
      // Search in SubNode (IndexBlock)
      std::optional<IndexEntry> retval = VisitIndexBlock(
          index_entry.GetSubNodeVCN(), file_name, visited_vcns, 0);
      if (retval) {
        return retval;
      }
    }
  }

  return {};
}

// Find Data attribute class of
template <Strategy S>
const AttrBase<S>* FileRecord<S>::FindStream(std::wstring_view name) const {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      GetAttr(AttrType::Data);
  for (const std::unique_ptr<AttrBase<S>>& data : vec) {
    // Unnamed stream
    if (data->IsUnNamed() && name.empty()) {
      LogDebug("FindStream() found the unnamed stream");
      return data.get();
    }
    // Named stream
    if ((!data->IsUnNamed()) && data->GetAttrName() == name) {
      LogDebug("FindStream() found stream named \"{}\"", WideToUtf8(name));
      return data.get();
    }
  }

  LogDebug("FindStream() found no stream named \"{}\"", WideToUtf8(name));
  return nullptr;
}

// Check if it's deleted or in use
template <Strategy S>
bool FileRecord<S>::IsDeleted() const noexcept {
  if (!impl_->file_record) {
    LogWarn("IsDeleted() called on a FileRecord with no parsed record");
    return false;
  }

  return !static_cast<bool>(impl_->file_record->GetData()->flags &
                            Flag::FileRecord::InUse);
}

// Check if it's a directory
template <Strategy S>
bool FileRecord<S>::IsDirectory() const noexcept {
  if (!impl_->file_record) {
    LogWarn("IsDirectory() called on a FileRecord with no parsed record");
    return false;
  }

  return static_cast<bool>(impl_->file_record->GetData()->flags &
                           Flag::FileRecord::Dir);
}

template <Strategy S>
bool FileRecord<S>::IsReadOnly() const noexcept {
  // Standard Information attribute holds the most updated file time
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsReadOnly();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsHidden() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsHidden();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsHidden();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSystem() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsSystem();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsSystem();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsArchive() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsArchive();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsArchive();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsDevice() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsDevice();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsDevice();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsNormal() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsNormal();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsNormal();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsTemporary() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsTemporary();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsTemporary();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsCompressed() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsCompressed();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsCompressed();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsOffline() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsOffline();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsOffline();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsNotContentIndexed() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsNotContentIndexed();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsNotContentIndexed();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsEncrypted() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsEncrypted();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSparse() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsSparse();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsSparse();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsReparsePoint() const noexcept {
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      std::get<AttrIndex(AttrType::StandardInformation)>(impl_->attr_list);
  if (vec.empty()) {
    return false;
  }

  if constexpr (S == Strategy::NoCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NoCache>*>(
               vec.front().get())
        ->IsReparsePoint();
  } else if constexpr (S == Strategy::FullCache) {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FullCache>*>(
               vec.front().get())
        ->IsReparsePoint();
  }
  return false;
}

template class FileRecord<Strategy::NoCache>;
template class FileRecord<Strategy::FullCache>;

}  // namespace NtfsBrowser
