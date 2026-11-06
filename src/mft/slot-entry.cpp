#include "mft/slot-entry.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc/claim-inventory.h"
#include "attr/file-name.h"
#include "io/file-record-attr-types.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Extents of $J the scan keeps at most, to announce them: the journal of a
// volume that was never trimmed can have many, and the loader reads them all.
// A longer list is cut, and the rest of $J is read without a hint.
constexpr size_t max_journal_extents = 4096;

// The Filename side of a $FILE_NAME attribute. FileRecord<S> always wraps a
// $FILE_NAME in the AttrFileName over the resident type that matches S.
template <Cache::Strategy S>
const Filename& AsFilename(const Attr::AttrBase<S>& attr) {
  return static_cast<const Attr::AttrFileName<Io::ResidentOf<S>, S>&>(attr);
}

// Appends each non-empty $FILE_NAME of file_record to entry.names, with its
// parent reference, parent sequence and DOS-only flag.
template <Cache::Strategy S>
void AddNames(const Io::FileRecord<S>& file_record,
              Mft::MftTree::Entry& entry) {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs =
      file_record.GetAttr(Attr::Type::FileName);
  entry.names.reserve(attrs.size());
  for (const std::unique_ptr<Attr::AttrBase<S>>& attr : attrs) {
    const Filename& filename = AsFilename<S>(*attr);
    const std::wstring_view name = filename.GetFilename();
    if (name.empty()) {
      continue;
    }

    Mft::MftTree::Name& added = entry.names.emplace_back();
    added.name = std::wstring(name);
    added.parent_record = filename.GetParentReference();
    added.parent_sequence = filename.GetParentSequenceNumber();
    added.dos_only = !filename.IsWin32Name();
  }
}

// Copies the names carved from file_record's slack into entry, and counts them.
// The record gives none unless the volume recovers and shows deleted items.
template <Cache::Strategy S>
void AddCarvedNames(const Io::FileRecord<S>& file_record,
                    Mft::MftTree::Entry& entry, RecordCounts& counts) {
  entry.carved_names = file_record.CarveSlackNames();
  counts.carved_names += entry.carved_names.size();
}

// Copies record's number, sequence number, liveness and kind into entry.
template <Cache::Strategy S>
void ReadIdentity(const Io::FileRecord<S>& file_record, ULONGLONG record,
                  Mft::MftTree::Entry& entry) {
  entry.record = record;
  entry.sequence = file_record.GetSequenceNumber();
  entry.in_use = !file_record.IsDeleted();
  entry.directory = file_record.IsDirectory();
}

// Copies the unnamed $DATA sizes into entry. Without that stream, the size is
// the $FILE_NAME size and nothing is allocated.
template <Cache::Strategy S>
void ReadStreamSizes(const Io::FileRecord<S>& file_record,
                     Mft::MftTree::Entry& entry) {
  const Attr::AttrBase<S>* data = file_record.FindStream({});
  entry.size =
      data != nullptr ? data->GetDataSize() : file_record.GetFileSize();
  entry.allocated_size = data != nullptr ? data->GetAllocatedSize() : 0;
}

// Copies the DOS permission bits of $STANDARD_INFORMATION into entry.
template <Cache::Strategy S>
void ReadPermissions(const Io::FileRecord<S>& file_record,
                     Mft::MftTree::Entry& entry) {
  entry.read_only = file_record.IsReadOnly();
  entry.hidden = file_record.IsHidden();
  entry.system = file_record.IsSystem();
  entry.archive = file_record.IsArchive();
  entry.compressed = file_record.IsCompressed();
  entry.encrypted = file_record.IsEncrypted();
  entry.sparse = file_record.IsSparse();
}

// Copies what file_record just parsed says about its file into entry.
// Returns false when an attribute failed to parse: entry then holds what
// parsed before the failure.
template <Cache::Strategy S>
bool ReadEntry(Io::FileRecord<S>& file_record, ULONGLONG record,
               Mft::MftTree::Entry& entry) {
  const bool parsed = TryParseAttrs(file_record);

  ReadIdentity(file_record, record, entry);
  AddNames(file_record, entry);
  ReadStreamSizes(file_record, entry);
  file_record.GetFileTime(&entry.write_time, &entry.create_time,
                          &entry.access_time, &entry.change_time);
  ReadPermissions(file_record, entry);

  return parsed;
}

// The clusters of $J when file_record is the live $UsnJrnl of $Extend, which
// the loader of the journal reads, and only when it will: the volume recovers.
// Empty otherwise. Reads nothing: the run list is in the record.
template <Cache::Strategy S>
std::vector<Alloc::Extent>
    JournalExtentsOf(const Io::FileRecord<S>& file_record,
                     const NtfsVolume<S>& volume,
                     const Mft::MftTree::Entry& entry) {
  const auto in_extend = [](const Mft::MftTree::Name& name) {
    return name.parent_record == static_cast<ULONGLONG>(Mft::Idx::Extend) &&
           name.name == L"$UsnJrnl";
  };
  if (!volume.GetOptions().recover_errors || !entry.in_use ||
      std::ranges::none_of(entry.names, in_extend)) {
    return {};
  }
  const Attr::AttrBase<S>* const stream = file_record.FindStream(L"$J");
  if (stream == nullptr) {
    return {};
  }
  std::vector<Alloc::Extent> extents = stream->GetExtents().extents;
  if (extents.size() > max_journal_extents) {
    extents.resize(max_journal_extents);
  }
  return extents;
}

}  // namespace

template <Cache::Strategy S>
bool TryParseAttrs(Io::FileRecord<S>& file_record) {
  try {
    return file_record.ParseAttrs();
  } catch (const std::exception& e) {
    Log::Exception(e);
    return false;
  }
}

template <Cache::Strategy S>
SlotKind ClassifySlot(Io::FileRecord<S>& file_record,
                      const NtfsVolume<S>& volume, bool parsed,
                      RecordCounts& counts, bool& header_damaged) {
  if (!parsed) {
    counts.unreadable++;
    if (file_record.HasFailedFixup()) {
      counts.damaged++;
    }
    return SlotKind::Unreadable;
  }
  header_damaged = file_record.HasFailedFixup();
  if (header_damaged) {
    counts.damaged++;
  }
  if (file_record.IsExtensionRecord()) {
    counts.extensions++;
    return SlotKind::Extension;
  }
  if (file_record.IsDeleted()) {
    counts.deleted++;
    if (!volume.GetOptions().include_deleted) {
      return SlotKind::Dropped;
    }
  } else {
    counts.in_use++;
  }
  return SlotKind::Base;
}

template <Cache::Strategy S>
bool ReadBaseEntry(Io::FileRecord<S>& file_record, const NtfsVolume<S>& volume,
                   const BaseSlot& slot, RecordCounts& counts,
                   Mft::MftTree::Entry& entry) {
  const bool parsed = ReadEntry(file_record, slot.record, entry);
  counts.reused_extensions += file_record.GetReusedExtensionCount();
  if (file_record.HasOverwrittenAttrList()) {
    counts.overwritten_lists++;
  }
  if (!parsed || file_record.IsSalvaged()) {
    if (!slot.header_damaged) {
      counts.damaged++;
    }
    // Strict: a damaged record is dropped, not kept partial.
    if (!parsed && !volume.GetOptions().recover_errors) {
      return false;
    }
  }
  AddCarvedNames(file_record, entry, counts);
  counts.journal_extents = JournalExtentsOf(file_record, volume, entry);
  return true;
}

template <Cache::Strategy S>
DataNote ComputeDataNote(const Io::FileRecord<S>& file_record) {
  DataNote note;
  const Attr::AttrBase<S>* data = file_record.FindStream({});
  if (data == nullptr) {
    return note;
  }
  note.kind = DataNote::Kind::Known;
  if (!file_record.IsDeleted()) {
    note.state = Alloc::DataState::Intact;
    return note;
  }

  Alloc::StreamExtents extents = data->GetExtents();
  if (extents.extents.empty()) {
    note.state =
        extents.complete ? Alloc::DataState::Intact : Alloc::DataState::Unknown;
    return note;
  }
  note.kind = DataNote::Kind::Query;
  note.extents = std::move(extents);
  return note;
}

template <Cache::Strategy S>
Alloc::RecordClaims ComputeClaims(const Io::FileRecord<S>& file_record,
                                  const BaseSlot& slot,
                                  const RecordCounts& counts) {
  const bool freed = file_record.IsDeleted();
  return Alloc::CollectClaims(file_record, slot.record, freed,
                              freed || slot.header_damaged ||
                                  counts.damaged != 0);
}

template bool TryParseAttrs<Cache::Strategy::NoCache>(
    Io::FileRecord<Cache::Strategy::NoCache>&);
template bool TryParseAttrs<Cache::Strategy::FullCache>(
    Io::FileRecord<Cache::Strategy::FullCache>&);

template SlotKind ClassifySlot<Cache::Strategy::NoCache>(
    Io::FileRecord<Cache::Strategy::NoCache>&,
    const NtfsVolume<Cache::Strategy::NoCache>&, bool, RecordCounts&, bool&);
template SlotKind ClassifySlot<Cache::Strategy::FullCache>(
    Io::FileRecord<Cache::Strategy::FullCache>&,
    const NtfsVolume<Cache::Strategy::FullCache>&, bool, RecordCounts&, bool&);

template bool ReadBaseEntry<Cache::Strategy::NoCache>(
    Io::FileRecord<Cache::Strategy::NoCache>&,
    const NtfsVolume<Cache::Strategy::NoCache>&, const BaseSlot&, RecordCounts&,
    Mft::MftTree::Entry&);
template bool ReadBaseEntry<Cache::Strategy::FullCache>(
    Io::FileRecord<Cache::Strategy::FullCache>&,
    const NtfsVolume<Cache::Strategy::FullCache>&, const BaseSlot&,
    RecordCounts&, Mft::MftTree::Entry&);

template DataNote ComputeDataNote<Cache::Strategy::NoCache>(
    const Io::FileRecord<Cache::Strategy::NoCache>&);
template DataNote ComputeDataNote<Cache::Strategy::FullCache>(
    const Io::FileRecord<Cache::Strategy::FullCache>&);

template Alloc::RecordClaims ComputeClaims<Cache::Strategy::NoCache>(
    const Io::FileRecord<Cache::Strategy::NoCache>&, const BaseSlot&,
    const RecordCounts&);
template Alloc::RecordClaims ComputeClaims<Cache::Strategy::FullCache>(
    const Io::FileRecord<Cache::Strategy::FullCache>&, const BaseSlot&,
    const RecordCounts&);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
