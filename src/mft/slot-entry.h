#pragma once

#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "alloc/types.h"
#include "cache/strategy.h"
#include "log/ntfs-common.h"
#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Mft {

// The counters one record slot adds to the scan's statistics. A slot parsed
// away from the thread that merges the scan keeps them apart, and they join the
// statistics when the scan reaches the slot: a slot that is parsed twice is
// counted once. The one slot that holds $UsnJrnl also tells where its $J lies.
struct RecordCounts {
  ULONGLONG unreadable{0};
  ULONGLONG damaged{0};
  ULONGLONG extensions{0};
  ULONGLONG deleted{0};
  ULONGLONG in_use{0};
  ULONGLONG reused_extensions{0};
  ULONGLONG overwritten_lists{0};
  ULONGLONG carved_names{0};
  // The clusters of the $J stream, for the loader of the journal to read them
  // ahead. Empty for every other slot.
  std::vector<Alloc::Extent> journal_extents;
};

// Adds counts to stats.
inline void AddCounts(Mft::MftTree::ScanStats& stats,
                      const RecordCounts& counts) noexcept {
  stats.unreadable += counts.unreadable;
  stats.damaged += counts.damaged;
  stats.extensions += counts.extensions;
  stats.deleted += counts.deleted;
  stats.in_use += counts.in_use;
  stats.reused_extensions += counts.reused_extensions;
  stats.overwritten_lists += counts.overwritten_lists;
  stats.carved_names += counts.carved_names;
}

// What one record slot holds, once its header is read.
enum class SlotKind : BYTE {
  Unreadable,  // Its header does not parse.
  Dropped,     // Freed and not asked for.
  Extension,   // An extension record: its base record imports its attributes.
  Base         // A base record, kept if its attributes allow it.
};

// A base record slot, and whether the update sequence check of its header
// failed: the header is then counted already.
struct BaseSlot {
  ULONGLONG record;
  bool header_damaged;
};

// The optional work a scan does for each entry it keeps.
struct SlotFeatures {
  // Gives each entry the data state of its unnamed $DATA stream.
  bool annotate{false};
  // Collects the clusters each entry claims, see ScanOptions::cluster_account.
  bool account{false};
};

// What the data state of one entry waits for, worked out from its record alone,
// so that a thread other than the scan's can do it. Only the scan's own thread
// asks the volume.
struct DataNote {
  // None: the record has no unnamed $DATA stream, and no state. Known: the
  // state is settled. Query: the clusters in extents decide it.
  enum class Kind : BYTE { None, Known, Query };
  Kind kind{Kind::None};
  Alloc::DataState state{Alloc::DataState::Unknown};
  Alloc::StreamExtents extents;
};

// What one record slot gave, parsed away from the thread that owns the scan and
// kept until the scan reaches the slot.
struct SlotOutcome {
  SlotKind kind{SlotKind::Unreadable};
  // The update sequence check of the header failed on some sector: the header
  // is counted already.
  bool header_damaged{false};
  // The base record number the slot names: 0 for a base record.
  ULONGLONG base_reference{0};
  // The thread that owns the scan MUST parse the slot itself, since it needs
  // the volume. Nothing else of the outcome counts then, and its log lines are
  // not replayed.
  bool needs_scan_thread{false};
  // A base record with an $ATTRIBUTE_LIST, which the scan holds for its second
  // pass, and the clusters of that list.
  bool has_list{false};
  std::vector<Alloc::Extent> list_extents;
  // A base record without one: whether it is kept, its entry and its data
  // state.
  bool kept{false};
  Mft::MftTree::Entry entry;
  DataNote note;
  RecordCounts counts;
  // The clusters the record claims, when the scan accounts for them and the
  // entry is kept.
  Alloc::RecordClaims claims;
  // The lines the parse logged, in the capture of the chunk it ran in, and then
  // the lines its attributes logged when they were dropped. With one thread,
  // the second kind comes later: when the next slot is parsed.
  Log::Detail::CapturedLog::Mark log_begin;
  Log::Detail::CapturedLog::Mark log_end;
  Log::Detail::CapturedLog::Mark release_end;
};

// Parses file_record's attributes. A raw callback may throw out of
// ParseAttrs(); that is a failed parse, logged here and not rethrown.
template <Cache::Strategy S>
bool TryParseAttrs(Io::FileRecord<S>& file_record);

// Classifies the record slot file_record holds, and counts it in counts.
// parsed is the verdict of the header parse. A record with a failed update
// sequence check is damaged in both modes: strict drops it as unreadable,
// recovering keeps what verifies. It is counted once, whatever else is wrong
// with it: header_damaged tells ReadBaseEntry() it is counted already.
template <Cache::Strategy S>
SlotKind ClassifySlot(Io::FileRecord<S>& file_record,
                      const NtfsVolume<S>& volume, bool parsed,
                      RecordCounts& counts, bool& header_damaged);

// Reads the base record file_record holds into entry, fresh. Returns false for
// a record not kept, a damaged one when strict: entry is then meaningless.
template <Cache::Strategy S>
bool ReadBaseEntry(Io::FileRecord<S>& file_record, const NtfsVolume<S>& volume,
                   const BaseSlot& slot, RecordCounts& counts,
                   Mft::MftTree::Entry& entry);

// Works out the DataNote of the unnamed $DATA stream of file_record.
template <Cache::Strategy S>
DataNote ComputeDataNote(const Io::FileRecord<S>& file_record);

// The claims of the record file_record holds, which ReadBaseEntry() just kept.
// The record is concerned when it is freed, or damaged: slot.header_damaged
// says its header was, counts.damaged that something else in it was.
template <Cache::Strategy S>
Alloc::RecordClaims ComputeClaims(const Io::FileRecord<S>& file_record,
                                  const BaseSlot& slot,
                                  const RecordCounts& counts);

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
