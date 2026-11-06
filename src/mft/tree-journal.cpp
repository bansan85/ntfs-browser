#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/usn/types.h>

#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/tree-impl.h"

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
bool Mft::MftTree::Impl::AnnounceJournal(
    const NtfsVolume<S>& volume,
    const std::vector<Alloc::Extent>& extents) const {
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

// Annotates the linked entries from the volume's change journal, when
// recovering and when the scan ran to its end.
template <Cache::Strategy S>
void Mft::MftTree::Impl::ApplyJournalIfComplete(const NtfsVolume<S>& volume,
                                                bool announced) {
  // A scan that progress() stopped holds only some records: a file would look
  // lost to the journal that is merely not read yet.
  if (!volume.GetOptions().recover_errors || !stats.complete) {
    return;
  }
  const Bench::PhaseScope phase(Bench::Phase::JournalLoad);
  volume.SetReadPattern(Tuning::Pattern::LargeSequential);
  const Usn::Journal* journal = volume.GetUsnJournal();
  // What the loader did not read of the announced journal is read for nothing.
  if (announced) {
    volume.CancelPrefetch();
  }
  if (journal == nullptr) {
    return;
  }
  ApplyJournal(*journal, volume.GetOptions().include_deleted);
  LogJournalSummary();
}

void Mft::MftTree::Impl::LogJournalSummary() const {
  Log::Info(
      "MFT scan journal: {} records, {} entries annotated, {} only in the "
      "journal",
      stats.usn_records, stats.usn_annotated, stats.usn_journal_only);
}

// Walks the journal in order, so the newest record of a file is the last one
// seen. A record names a file by record number and sequence. It belongs to an
// entry when the sequence is that entry's generation (see
// IsSameRecordGeneration()); otherwise it names a file that no entry accounts
// for: one whose record is gone, or was reused. A V3 reference with a high half
// cannot name an $MFT record, so it never matches one.
void Mft::MftTree::Impl::ApplyJournal(const Usn::Journal& journal,
                                      bool include_deleted) {
  stats.usn_records = journal.records.size();
  stats.usn_damaged = journal.damaged_records;
  stats.usn_truncated = journal.truncated;

  // Record number, sequence number, high half of the reference.
  using Key = std::tuple<ULONGLONG, WORD, ULONGLONG>;
  std::map<Key, Mft::MftTree::JournalOnlyEntry> lost;

  for (const Usn::Record& record : journal.records) {
    if (record.file_reference_high == 0) {
      if (const std::optional<size_t> index = EntryIndex(record.file_record)) {
        // EntryIndex() gives an index of entries.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        Mft::MftTree::Entry& entry = entries[*index];
        if (Mft::IsSameRecordGeneration(record.file_sequence, entry.sequence,
                                        entry.in_use)) {
          if (!entry.usn) {
            entry.usn = Mft::MftTree::UsnInfo{};
            stats.usn_annotated++;
          }
          NoteRecord(*entry.usn, record);
          continue;
        }
      }
    }
    if (!include_deleted) {
      continue;
    }

    Mft::MftTree::JournalOnlyEntry& item = lost[Key{
        record.file_record, record.file_sequence, record.file_reference_high}];
    item.record = record.file_record;
    item.sequence = record.file_sequence;
    item.name = record.name;
    item.parent_record = record.parent_record;
    item.parent_sequence = record.parent_sequence;
    item.file_attributes = record.file_attributes;
    NoteRecord(item.usn, record);
  }

  for (auto& [key, item] : lost) {
    item.parent_valid =
        NamesDirectory(item.parent_record, item.parent_sequence);
    item.directory = (item.file_attributes & journal_directory_attribute) != 0;
    journal_only.push_back(std::move(item));
  }
  stats.usn_journal_only = journal_only.size();
}

template bool Mft::MftTree::Impl::AnnounceJournal<Cache::Strategy::NoCache>(
    const NtfsVolume<Cache::Strategy::NoCache>&,
    const std::vector<Alloc::Extent>&) const;
template bool Mft::MftTree::Impl::AnnounceJournal<Cache::Strategy::FullCache>(
    const NtfsVolume<Cache::Strategy::FullCache>&,
    const std::vector<Alloc::Extent>&) const;

template void
    Mft::MftTree::Impl::ApplyJournalIfComplete<Cache::Strategy::NoCache>(
        const NtfsVolume<Cache::Strategy::NoCache>&, bool);
template void
    Mft::MftTree::Impl::ApplyJournalIfComplete<Cache::Strategy::FullCache>(
        const NtfsVolume<Cache::Strategy::FullCache>&, bool);

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
