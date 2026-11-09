#pragma once

#include <map>
#include <tuple>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "cache/strategy.h"
#include "mft/entry-table.h"
#include "mft/tree.h"
#include "usn/types.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Announces the clusters of $J to the reader, in the order the loader of the
// journal reads them. Returns whether it announced any.
template <Cache::Strategy S>
bool AnnounceJournal(const NtfsVolume<S>& volume,
                     const std::vector<Alloc::Extent>& extents);

// Reads the change journal against the entries of a table: gives each entry
// the journal names its UsnInfo, and lists the files it names that no entry
// accounts for. The table MUST outlive this object.
class JournalAnnotator {
 public:
  // With include_deleted, the files only the journal names are listed.
  JournalAnnotator(EntryTable& table, bool include_deleted) noexcept;

  // Walks the journal in order, so the newest record of a file is the last one
  // seen. A record names a file by record number and sequence. It belongs to
  // an entry when the sequence is that entry's generation (see
  // IsSameRecordGeneration()); otherwise it names a file that no entry
  // accounts for: one whose record is gone, or was reused. A V3 reference with
  // a high half cannot name an $MFT record, so it never matches one. Sets the
  // journal counters of stats. Returns the files only the journal names,
  // ordered by record, then sequence.
  [[nodiscard]] std::vector<Mft::MftTree::JournalOnlyEntry>
      Apply(const Usn::Journal& journal, Mft::MftTree::ScanStats& stats);

 private:
  // Record number, sequence number, high half of the reference.
  using Key = std::tuple<ULONGLONG, WORD, ULONGLONG>;

  // Gives the entry of the record the journal names its UsnInfo. False when no
  // entry has that generation.
  bool NoteOnEntry(const Usn::Record& record, Mft::MftTree::ScanStats& stats);
  // Adds the record to the file no entry accounts for.
  void NoteLost(const Usn::Record& record);

  EntryTable& table_;
  bool include_deleted_;
  std::map<Key, Mft::MftTree::JournalOnlyEntry> lost_;
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
