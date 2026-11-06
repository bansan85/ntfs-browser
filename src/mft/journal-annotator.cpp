#include "mft/journal-annotator.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/usn/types.h>

#include "mft/entry-table.h"
#include "mft/file-reference.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Ranges announced for $J at most: 1 GiB, far more than the default journal of
// 32 MiB. The reader keeps 1024 ranges and drops a hint past them.
constexpr size_t max_journal_pieces = 256;

// FILE_ATTRIBUTE_DIRECTORY, in the attributes a journal record carries.
constexpr DWORD journal_directory_attribute = 0x10;

// Adds one journal record to what is known of a file.
void NoteRecord(Mft::MftTree::UsnInfo& info, const Usn::Record& record) {
  info.last_usn = record.usn;
  info.last_reason = record.reason;
  info.last_time = record.time;
  info.all_reasons |= record.reason;
  info.record_count++;
  if ((record.reason & static_cast<DWORD>(Usn::Reason::FileDelete)) != 0) {
    info.delete_time = record.time;
  }
}

}  // namespace

template <Cache::Strategy S>
bool AnnounceJournal(const NtfsVolume<S>& volume,
                     const std::vector<Alloc::Extent>& extents) {
  const ULONGLONG cluster_size = volume.GetClusterSize();
  // A byte address MUST fit a LONGLONG, as in ReadClusters().
  const ULONGLONG max_lcn =
      cluster_size == 0
          ? 0
          : static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()) /
                cluster_size;
  size_t pieces = 0;
  const ULONGLONG piece_bytes =
      (std::max<ULONGLONG>)(volume.GetOptions().tuning.mft.journal_piece_bytes,
                            cluster_size);
  for (const Alloc::Extent& extent : extents) {
    if (extent.lcn > max_lcn || extent.clusters > max_lcn - extent.lcn) {
      return pieces != 0;
    }
    const ULONGLONG end = (extent.lcn + extent.clusters) * cluster_size;
    for (ULONGLONG at = extent.lcn * cluster_size; at < end;
         at += piece_bytes) {
      if (pieces == max_journal_pieces) {
        return true;
      }
      pieces++;
      volume.Prefetch(at, (std::min)(piece_bytes, end - at));
    }
  }
  return pieces != 0;
}

JournalAnnotator::JournalAnnotator(EntryTable& table,
                                   bool include_deleted) noexcept
    : table_(table), include_deleted_(include_deleted) {}

std::vector<Mft::MftTree::JournalOnlyEntry>
    JournalAnnotator::Apply(const Usn::Journal& journal,
                            Mft::MftTree::ScanStats& stats) {
  stats.usn_records = journal.records.size();
  stats.usn_damaged = journal.damaged_records;
  stats.usn_truncated = journal.truncated;

  for (const Usn::Record& record : journal.records) {
    if (NoteOnEntry(record, stats)) {
      continue;
    }
    if (include_deleted_) {
      NoteLost(record);
    }
  }

  std::vector<Mft::MftTree::JournalOnlyEntry> journal_only;
  for (auto& [key, item] : lost_) {
    item.parent_valid =
        table_.NamesDirectory(item.parent_record, item.parent_sequence);
    item.directory = (item.file_attributes & journal_directory_attribute) != 0;
    journal_only.push_back(std::move(item));
  }
  stats.usn_journal_only = journal_only.size();
  return journal_only;
}

bool JournalAnnotator::NoteOnEntry(const Usn::Record& record,
                                   Mft::MftTree::ScanStats& stats) {
  if (record.file_reference_high != 0) {
    return false;
  }
  const std::optional<size_t> index = table_.IndexOf(record.file_record);
  if (!index) {
    return false;
  }
  // IndexOf() gives an index of the entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  Mft::MftTree::Entry& entry = table_.Entries()[*index];
  if (!Mft::IsSameRecordGeneration(record.file_sequence, entry.sequence,
                                   entry.in_use)) {
    return false;
  }
  if (!entry.usn) {
    entry.usn = Mft::MftTree::UsnInfo{};
    stats.usn_annotated++;
  }
  NoteRecord(*entry.usn, record);
  return true;
}

void JournalAnnotator::NoteLost(const Usn::Record& record) {
  Mft::MftTree::JournalOnlyEntry& item = lost_[Key{
      record.file_record, record.file_sequence, record.file_reference_high}];
  item.record = record.file_record;
  item.sequence = record.file_sequence;
  item.name = record.name;
  item.parent_record = record.parent_record;
  item.parent_sequence = record.parent_sequence;
  item.file_attributes = record.file_attributes;
  NoteRecord(item.usn, record);
}

template bool AnnounceJournal<Cache::Strategy::NoCache>(
    const NtfsVolume<Cache::Strategy::NoCache>&,
    const std::vector<Alloc::Extent>&);
template bool AnnounceJournal<Cache::Strategy::FullCache>(
    const NtfsVolume<Cache::Strategy::FullCache>&,
    const std::vector<Alloc::Extent>&);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
