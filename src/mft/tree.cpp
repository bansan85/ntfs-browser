#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>

#include "alloc/data-state.h"
#include "attr/file-name.h"
#include "io/file-record-attr-types.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/slot-reader.h"
#include "mft/tree-testing.h"
#include "ntfs-browser/record/carved-name.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Attr {

template <Cache::Strategy S>
class AttrBase;
template <typename Resident, Cache::Strategy S>
class AttrFileName;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The root directory's record number, fixed by NTFS.
constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// Records between two progress() calls: often enough for a live progress
// line, rare enough to cost nothing next to a record read.
constexpr ULONGLONG progress_interval = 4096;

// Bits in one byte of the $MFT bitmap.
constexpr ULONGLONG bits_per_byte = 8;

// Data-state queries a scan keeps pending before it resolves them. A RangeQuery
// is 40 bytes, so this is about 10 MiB: enough to batch thousands of files, so
// the bitmap is read once per batch and not once per file, and small enough to
// bound the memory of a scan whatever the volume holds.
constexpr size_t default_max_pending_ranges = 262144;

// The cap a test asked for. 0 means the default.
size_t max_pending_ranges_override = 0;

// Free slots a block read may span between two used ones. 64 slots are 64 KiB
// at the usual 1 KiB record: a disk seek costs more than reading that much.
constexpr ULONGLONG max_gap_slots = 64;

// The entry index of a record the scan did not keep.
constexpr size_t no_entry = std::numeric_limits<size_t>::max();

// Bytes of raw records a scan holds for its second pass at most. Past it, a
// base record reads its extension records from the disk, one at a time. A big
// system volume has about 100 000 extension records of 1 KiB: 64 bits leave
// room for several times that, 32 bits keep most of the address space free.
constexpr size_t default_max_held_record_bytes =
    sizeof(void*) >= 8 ? size_t{512} << 20 : size_t{128} << 20;

// The cap a test asked for. nullopt means the default.
std::optional<size_t> max_held_record_bytes_override;

// Bytes of one block of held records. Records are held in blocks, so holding
// one more never moves those already held.
constexpr size_t held_block_bytes = size_t{4} << 20;

// Records the dense index of a tree covers whatever its entries: 2^20 slots
// are 8 MiB, little next to the entries of a volume that large.
constexpr size_t dense_index_floor = size_t{1} << 20;

// Records the dense index covers per kept entry beyond the floor. Even a
// $MFT with seven free slots in eight stays dense, and a forged one far
// sparser falls back to the map instead of growing the index.
constexpr size_t dense_index_ratio = 8;

// Entries a scan reserves room for at most, from the count the $MFT bitmap
// gives. 2^20 covers most volumes, and bounds what a forged bitmap makes a
// scan allocate before it reads a record: about 200 MiB.
constexpr ULONGLONG max_reserved_entries = ULONGLONG{1} << 20;

// The Filename side of a $FILE_NAME attribute. FileRecord<S> always wraps a
// $FILE_NAME in the AttrFileName over the resident type that matches S.
template <Cache::Strategy S>
const Filename& AsFilename(const Attr::AttrBase<S>& attr) {
  return static_cast<const Attr::AttrFileName<Io::ResidentOf<S>, S>&>(attr);
}

// Parses file_record's attributes. A raw callback may throw out of
// ParseAttrs(); that is a failed parse, logged here and not rethrown.
template <Cache::Strategy S>
bool TryParseAttrs(Io::FileRecord<S>& file_record) {
  try {
    return file_record.ParseAttrs();
  } catch (const std::exception& e) {
    Log::Exception(e);
    return false;
  }
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
                    Mft::MftTree::Entry& entry,
                    Mft::MftTree::ScanStats& stats) {
  entry.carved_names = file_record.CarveSlackNames();
  stats.carved_names += entry.carved_names.size();
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

// Reads the unnamed $BITMAP of $MFT: one bit per record slot, set when the
// slot is in use. Returns nullopt, and the scan then reads every slot, when
// record 0 or its bitmap cannot be read or the bitmap does not cover total
// slots.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>> ReadMftBitmap(const NtfsVolume<S>& volume,
                                               ULONGLONG total) {
  try {
    Io::FileRecord<S> mft_record(volume);
    mft_record.SetAttrMask(Attr::Mask::Bitmap);
    if (!mft_record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Mft)) ||
        !TryParseAttrs(mft_record)) {
      return std::nullopt;
    }

    const ULONGLONG needed = (total + bits_per_byte - 1) / bits_per_byte;
    for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
         mft_record.GetAttr(Attr::Type::Bitmap)) {
      if (!attr->IsUnNamed() || attr->GetDataSize() < needed) {
        continue;
      }
      std::vector<BYTE> bits(gsl::narrow<size_t>(needed));
      const std::optional<ULONGLONG> read =
          attr->ReadData(0, {bits.data(), bits.size()});
      if (read && *read == needed) {
        return bits;
      }
    }
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  return std::nullopt;
}

// Slots bitmap marks in use among the total it covers.
ULONGLONG CountUsedSlots(const std::vector<BYTE>& bitmap,
                         ULONGLONG total) noexcept {
  ULONGLONG used = 0;
  for (const BYTE bits : bitmap) {
    used += static_cast<ULONGLONG>(std::popcount(bits));
  }
  // The bits of the last byte past total cover no slot.
  const auto tail_bits = static_cast<unsigned>(total % bits_per_byte);
  if (tail_bits != 0 && !bitmap.empty()) {
    const auto past_total =
        static_cast<BYTE>(bitmap.back() & ~((1U << tail_bits) - 1U));
    used -= static_cast<ULONGLONG>(std::popcount(past_total));
  }
  return used;
}

// Whether bitmap marks record as free. The bitmap covers every record the
// scan visits, so record / 8 is in range.
bool IsMarkedFree(const std::vector<BYTE>& bitmap, ULONGLONG record) noexcept {
  // ReadMftBitmap() sized bitmap to cover all slots, and record is one of them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const BYTE bits = bitmap[record / bits_per_byte];
  return (bits & (1U << (record % bits_per_byte))) == 0;
}

// End (exclusive) of the slots one block read serves, from the used slot
// first: up to the last used slot before a gap of more than max_gap_slots free
// ones, or total. The free slots inside are read with it, but not parsed.
ULONGLONG ReadRangeEnd(const std::vector<BYTE>& bitmap, ULONGLONG first,
                       ULONGLONG total) noexcept {
  ULONGLONG last_used = first;
  for (ULONGLONG slot = first + 1;
       slot < total && slot - last_used <= max_gap_slots; slot++) {
    if (!IsMarkedFree(bitmap, slot)) {
      last_used = slot;
    }
  }
  return last_used + 1;
}

// The ranges of slots the scan reads with a bitmap: each starts at a used slot
// and ends where ReadRangeEnd() says.
std::vector<Mft::SlotRange> PlanSlotRanges(const std::vector<BYTE>& bitmap,
                                           ULONGLONG total) {
  std::vector<Mft::SlotRange> plan;
  ULONGLONG record = 0;
  while (record < total) {
    if (IsMarkedFree(bitmap, record)) {
      record++;
      continue;
    }
    const ULONGLONG end = ReadRangeEnd(bitmap, record, total);
    plan.push_back({.first = record, .last = end});
    record = end;
  }
  return plan;
}

// Whether the scan goes on at record. Every progress_interval records, the
// caller follows along through options.progress and may stop the scan.
bool ProgressAllowsRecord(const Mft::MftTree::ScanOptions& options,
                          ULONGLONG record, ULONGLONG total) {
  return !options.progress || record % progress_interval != 0 ||
         options.progress(record, total);
}

// Raw $MFT records a scan holds for its second pass, as the slot reader read
// them: the extension records it met, and the base records that wait for
// theirs. Records come in increasing order. Both kinds share one budget,
// MaxHeldRecordBytes().
class HeldRecords {
 public:
  // A base record that waits for the second pass, with the entry kept in its
  // place, and what its first header read counted.
  struct Waiting {
    ULONGLONG record;
    size_t entry_index;
    bool header_damaged;
    size_t slot;
  };

  explicit HeldRecords(size_t record_size)
      : record_size_(record_size),
        block_slots_(record_size != 0
                         ? (std::max)(size_t{1}, held_block_bytes / record_size)
                         : 1),
        budget_(MaxHeldRecordBytes()) {}

  // Holds extension record. False when bytes is empty or the budget is spent.
  bool HoldExtension(ULONGLONG record, std::span<const BYTE> bytes) {
    size_t slot = 0;
    if (!Store(bytes, slot)) {
      return false;
    }
    extensions_.push_back({.record = record, .slot = slot});
    return true;
  }

  // Holds base record for the second pass. False when bytes is empty or the
  // budget is spent.
  bool HoldWaiting(ULONGLONG record, std::span<const BYTE> bytes,
                   size_t entry_index, bool header_damaged) {
    size_t slot = 0;
    if (!Store(bytes, slot)) {
      return false;
    }
    waiting_.push_back({.record = record,
                        .entry_index = entry_index,
                        .header_damaged = header_damaged,
                        .slot = slot});
    return true;
  }

  // The bytes of extension record, or an empty span when it is not held.
  [[nodiscard]] std::span<const BYTE> FindExtension(ULONGLONG record) const {
    const auto found =
        std::ranges::lower_bound(extensions_, record, {}, &Extension::record);
    if (found == extensions_.end() || found->record != record) {
      return {};
    }
    return Bytes(found->slot);
  }

  [[nodiscard]] const std::vector<Waiting>& WaitingRecords() const noexcept {
    return waiting_;
  }

  // The bytes held in slot, which Store() gave.
  [[nodiscard]] std::span<const BYTE> Bytes(size_t slot) const {
    const std::vector<BYTE>& block = blocks_.at(slot / block_slots_);
    return std::span<const BYTE>{block}.subspan(
        (slot % block_slots_) * record_size_, record_size_);
  }

 private:
  struct Extension {
    ULONGLONG record;
    size_t slot;
  };

  // Copies bytes, one record, into the next slot. False when bytes is not a
  // record or the budget is spent.
  bool Store(std::span<const BYTE> bytes, size_t& slot) {
    if (bytes.size() != record_size_ || (held_ + 1) * record_size_ > budget_) {
      return false;
    }
    if (held_ % block_slots_ == 0) {
      blocks_.emplace_back().reserve(block_slots_ * record_size_);
    }
    std::vector<BYTE>& block = blocks_.back();
    block.insert(block.end(), bytes.begin(), bytes.end());
    slot = held_++;
    return true;
  }

  size_t record_size_;
  // Records in one block.
  size_t block_slots_;
  // Bytes this scan may hold.
  size_t budget_;
  // Records held so far, both kinds.
  size_t held_{0};
  std::vector<std::vector<BYTE>> blocks_;
  // Sorted by record: the scan meets them in that order.
  std::vector<Extension> extensions_;
  std::vector<Waiting> waiting_;
};

// What one record slot holds, once its header is read.
enum class SlotKind : BYTE {
  Dropped,    // Unreadable, or freed and not asked for.
  Extension,  // An extension record: its base record imports its attributes.
  Base        // A base record, kept if its attributes allow it.
};

// Reads the header of one record slot into file_record, and counts it in
// stats. A record with a failed update sequence check is damaged in both
// modes: strict drops it as unreadable, recovering keeps what verifies. It is
// counted once, whatever else is wrong with it: header_damaged tells
// ReadBaseEntry() it is counted already.
template <Cache::Strategy S>
SlotKind ReadSlotHeader(Io::FileRecord<S>& file_record,
                        Mft::SlotReader<S>& slots, const NtfsVolume<S>& volume,
                        ULONGLONG record, Mft::MftTree::ScanStats& stats,
                        bool& header_damaged) {
  if (!slots.ParseFileRecord(file_record, record)) {
    stats.unreadable++;
    if (file_record.HasFailedFixup()) {
      stats.damaged++;
    }
    return SlotKind::Dropped;
  }
  header_damaged = file_record.HasFailedFixup();
  if (header_damaged) {
    stats.damaged++;
  }
  if (file_record.IsExtensionRecord()) {
    stats.extensions++;
    return SlotKind::Extension;
  }
  if (file_record.IsDeleted()) {
    stats.deleted++;
    if (!volume.GetOptions().include_deleted) {
      return SlotKind::Dropped;
    }
  } else {
    stats.in_use++;
  }
  return SlotKind::Base;
}

// Reads the base record file_record holds into entry, fresh. Returns false for
// a record not kept, a damaged one when strict: entry is then meaningless.
template <Cache::Strategy S>
bool ReadBaseEntry(Io::FileRecord<S>& file_record, const NtfsVolume<S>& volume,
                   ULONGLONG record, bool header_damaged,
                   Mft::MftTree::ScanStats& stats, Mft::MftTree::Entry& entry) {
  const bool parsed = ReadEntry(file_record, record, entry);
  if (!parsed || file_record.IsSalvaged()) {
    if (!header_damaged) {
      stats.damaged++;
    }
    // Strict: a damaged record is dropped, not kept partial.
    if (!parsed && !volume.GetOptions().recover_errors) {
      return false;
    }
  }
  AddCarvedNames(file_record, entry, stats);
  return true;
}

// Removes the entries at indices, sorted, and keeps the others in order.
void DropEntries(std::vector<Mft::MftTree::Entry>& entries,
                 const std::vector<size_t>& indices) {
  if (indices.empty()) {
    return;
  }
  size_t kept = 0;
  size_t next_drop = 0;
  for (size_t index = 0; index < entries.size(); index++) {
    if (next_drop < indices.size() && indices.at(next_drop) == index) {
      next_drop++;
      continue;
    }
    if (kept != index) {
      entries.at(kept) = std::move(entries.at(index));
    }
    kept++;
  }
  entries.erase(std::next(entries.begin(), gsl::narrow<std::ptrdiff_t>(kept)),
                entries.end());
}

// The name a record's path goes through by default: the first non-DOS name
// with a valid parent, else the first non-DOS name, else the first name.
const Mft::MftTree::Name*
    PrimaryName(const Mft::MftTree::Entry& entry) noexcept {
  const Mft::MftTree::Name* fallback = nullptr;
  for (const Mft::MftTree::Name& name : entry.names) {
    if (name.dos_only) {
      continue;
    }
    if (name.parent_valid) {
      return &name;
    }
    if (fallback == nullptr) {
      fallback = &name;
    }
  }
  if (fallback != nullptr) {
    return fallback;
  }
  return entry.names.empty() ? nullptr : &entry.names.front();
}

// Whether entry.names[index] files entry under its parent. Only a valid,
// non-DOS name files a record, the root is filed under nothing, and two hard
// links in one directory file the record there once.
bool FilesUnderParent(const Mft::MftTree::Entry& entry, size_t index) {
  const auto files = [&entry](const Mft::MftTree::Name& name) {
    return name.parent_valid && !name.dos_only && entry.record != root_record;
  };
  const Mft::MftTree::Name& name = entry.names.at(index);
  if (!files(name)) {
    return false;
  }
  return std::ranges::none_of(
      entry.names.begin(),
      std::next(entry.names.begin(), gsl::narrow<std::ptrdiff_t>(index)),
      [&](const Mft::MftTree::Name& earlier) {
        return files(earlier) && earlier.parent_record == name.parent_record;
      });
}

// Gives the entries of one scan their data state. A deleted stream's extents
// are kept as queries, and resolved against the cluster bitmap in one sorted
// batch once enough of them wait, and at the end: deleted files sit at random
// clusters, so a query per file would reload a window of the bitmap for nearly
// each of them.
template <Cache::Strategy S>
class DataStateResolver {
 public:
  DataStateResolver(const NtfsVolume<S>& volume,
                    std::vector<Mft::MftTree::Entry>& entries,
                    Mft::MftTree::ScanStats& stats)
      : volume_(&volume), entries_(&entries), stats_(&stats) {}

  // Handles the entry at entry_index, which file_record has just filled.
  void Note(const Io::FileRecord<S>& file_record, size_t entry_index) {
    const Attr::AttrBase<S>* data = file_record.FindStream({});
    if (data == nullptr) {
      return;
    }
    if (!file_record.IsDeleted()) {
      Set(entry_index, Alloc::DataState::Intact);
      return;
    }

    const Alloc::StreamExtents extents = data->GetExtents();
    if (extents.extents.empty()) {
      Set(entry_index, extents.complete ? Alloc::DataState::Intact
                                        : Alloc::DataState::Unknown);
      return;
    }
    waiting_.push_back({.entry_index = entry_index,
                        .first_query = queries_.size(),
                        .query_count = extents.extents.size(),
                        .complete = extents.complete});
    for (const Alloc::Extent& extent : extents.extents) {
      queries_.push_back({.lcn = extent.lcn, .clusters = extent.clusters});
    }
    if (queries_.size() >= MaxPendingRanges()) {
      Flush();
    }
  }

  // Resolves every waiting entry. A bitmap that cannot answer leaves them
  // unknown: the scan goes on.
  void Flush() {
    if (waiting_.empty()) {
      return;
    }
    volume_->GetRangeStates(queries_);

    std::vector<Alloc::ClusterState> states;
    for (const Waiting& entry : waiting_) {
      states.clear();
      for (size_t i = 0; i < entry.query_count; i++) {
        states.push_back(queries_.at(entry.first_query + i).state);
      }
      Set(entry.entry_index, Alloc::FoldDataState(entry.complete, states));
    }
    waiting_.clear();
    queries_.clear();
  }

 private:
  // An entry whose extents wait in queries_.
  struct Waiting {
    size_t entry_index;
    size_t first_query;
    size_t query_count;
    bool complete;
  };

  void Set(size_t entry_index, Alloc::DataState state) {
    entries_->at(entry_index).data_state = state;
    switch (state) {
      case Alloc::DataState::Intact:
        stats_->data_intact++;
        break;
      case Alloc::DataState::Reallocated:
        stats_->data_reallocated++;
        break;
      case Alloc::DataState::Unknown:
        stats_->data_unknown++;
        break;
      case Alloc::DataState::Bad:
        stats_->data_bad++;
        break;
    }
  }

  const NtfsVolume<S>* volume_;
  std::vector<Mft::MftTree::Entry>* entries_;
  Mft::MftTree::ScanStats* stats_;
  std::vector<Alloc::RangeQuery> queries_;
  std::vector<Waiting> waiting_;
};

}  // namespace

size_t MaxPendingRanges() {
  return max_pending_ranges_override != 0 ? max_pending_ranges_override
                                          : default_max_pending_ranges;
}

size_t MaxHeldRecordBytes() {
  return max_held_record_bytes_override.value_or(default_max_held_record_bytes);
}

void SetMaxHeldRecordBytesForTests(std::optional<size_t> limit) {
  max_held_record_bytes_override = limit;
}

void SetMaxPendingRangesForTests(size_t limit) {
  max_pending_ranges_override = limit;
}

class MftTree::Impl {
 public:
  std::vector<Mft::MftTree::Entry> entries;
  // Index in entries of each kept record, by record number, no_entry for the
  // others. It ends after the last record it holds. A record that would grow
  // it past what the entries justify sits in sparse_entries instead.
  std::vector<size_t> entry_of_record;
  std::unordered_map<ULONGLONG, size_t> sparse_entries;
  // The records filed under each entry, through a valid parent reference:
  // those of entries[i] are children_records[children_first[i]] up to
  // children_first[i + 1]. A root whose record is lost has its own list.
  std::vector<size_t> children_first;
  std::vector<ULONGLONG> children_records;
  std::vector<ULONGLONG> lost_root_children;
  // Parallel to entries_.
  std::vector<bool> reachable;
  Mft::MftTree::ScanStats stats;
  // The files only the journal names, ordered by record, then sequence.
  std::vector<Mft::MftTree::JournalOnlyEntry> journal_only;

  template <Cache::Strategy S>
  void Scan(const NtfsVolume<S>& volume,
            const Mft::MftTree::ScanOptions& options);
  template <Cache::Strategy S>
  void ReadSlots(const NtfsVolume<S>& volume,
                 const Mft::MftTree::ScanOptions& options,
                 const std::optional<std::vector<BYTE>>& bitmap, bool annotate);
  void Link();
  // Files every entry under the parents its valid names give.
  void FileChildren();
  // Records that entries[index] holds record.
  void IndexEntry(ULONGLONG record, size_t index);
  // Index in entries of record, or nothing when the scan did not keep it.
  [[nodiscard]] std::optional<size_t> EntryIndex(ULONGLONG record) const;
  template <Cache::Strategy S>
  void ApplyJournalIfComplete(const NtfsVolume<S>& volume);
  void LogScanSummary(bool annotated) const;
  // Gives each entry the journal names its UsnInfo, and, with include_deleted,
  // lists the files it names that no entry accounts for.
  void ApplyJournal(const Usn::Journal& journal, bool include_deleted);
  [[nodiscard]] const Mft::MftTree::Entry* Find(ULONGLONG record) const;
  [[nodiscard]] std::span<const ULONGLONG> Children(ULONGLONG dir_record) const;
  [[nodiscard]] bool IsValidParent(const Mft::MftTree::Entry& child,
                                   const Mft::MftTree::Name& name) const;
  // Whether a parent reference names a directory the scan found, under the
  // same rules as IsValidParent(), minus the case of a record filed under
  // itself.
  [[nodiscard]] bool NamesDirectory(ULONGLONG parent_record,
                                    WORD parent_sequence) const;
  [[nodiscard]] std::wstring
      PathThrough(const Mft::MftTree::Entry& entry,
                  const Mft::MftTree::Name& name,
                  std::optional<ULONGLONG>* lost_ancestor) const;

 private:
  // The path of a directory through its primary name, as PathThrough() builds
  // it, and the record where its chain breaks.
  struct DirectoryPath {
    std::wstring path;
    std::optional<ULONGLONG> lost;
  };

  [[nodiscard]] const DirectoryPath&
      PathOfDirectory(const Mft::MftTree::Entry& directory,
                      const Mft::MftTree::Name& primary) const;
  [[nodiscard]] std::wstring WalkPath(const Mft::MftTree::Entry& entry,
                                      const Mft::MftTree::Name& name,
                                      std::optional<ULONGLONG>& lost) const;

  // The directories PathThrough() went through, by record.
  mutable std::unordered_map<ULONGLONG, DirectoryPath> directory_paths_;
};

Mft::MftTree::MftTree(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                      const Mft::MftTree::ScanOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->Scan(volume, options);
}

Mft::MftTree::MftTree(const NtfsVolume<Cache::Strategy::FullCache>& volume,
                      const Mft::MftTree::ScanOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->Scan(volume, options);
}

Mft::MftTree::MftTree(const Mft::MftTree& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

Mft::MftTree::MftTree(Mft::MftTree&& other) noexcept = default;

Mft::MftTree& Mft::MftTree::operator=(const Mft::MftTree& other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}

Mft::MftTree& Mft::MftTree::operator=(Mft::MftTree&& other) noexcept = default;

Mft::MftTree::~MftTree() = default;

// Reads the record slots of volume's $MFT into entries, in record order,
// through one reused FileRecord. A base record with an $ATTRIBUTE_LIST waits
// for a second pass: by then the first one has held every extension record it
// met, so they come from memory instead of one disk read each.
template <Cache::Strategy S>
void Mft::MftTree::Impl::ReadSlots(
    const NtfsVolume<S>& volume, const Mft::MftTree::ScanOptions& options,
    const std::optional<std::vector<BYTE>>& bitmap, bool annotate) {
  const ULONGLONG total = volume.GetRecordsCount();
  HeldRecords held(volume.GetFileRecordSize());
  Io::FileRecord<S> file_record(volume);
  file_record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
  file_record.SetExtensionSource(
      [&held](ULONGLONG record) { return held.FindExtension(record); });
  DataStateResolver<S> resolver(volume, entries, stats);
  // Reads $MFT in big blocks, not record by record, and only the used ones.
  Mft::SlotReader<S> slots(volume);
  if (bitmap) {
    slots.Plan(PlanSlotRanges(*bitmap, total));
  }

  for (ULONGLONG record = 0; record < total; record++) {
    if (!ProgressAllowsRecord(options, record, total)) {
      stats.complete = false;
      break;
    }

    if (bitmap && IsMarkedFree(*bitmap, record)) {
      stats.skipped++;
      continue;
    }

    bool header_damaged = false;
    const SlotKind kind = ReadSlotHeader(file_record, slots, volume, record,
                                         stats, header_damaged);
    if (kind == SlotKind::Extension) {
      // Its base record may come later, and then takes it from memory.
      static_cast<void>(held.HoldExtension(record, slots.HeldSlot(record)));
      continue;
    }
    if (kind == SlotKind::Dropped) {
      continue;
    }
    // Its entry keeps its place, so the entries stay in record order.
    if (file_record.HasAttrHeader(Attr::Type::AttributeList) &&
        held.HoldWaiting(record, slots.HeldSlot(record), entries.size(),
                         header_damaged)) {
      entries.emplace_back();
      continue;
    }

    // Filled in place: an Entry is big, and most slots are kept.
    Mft::MftTree::Entry& entry = entries.emplace_back();
    if (!ReadBaseEntry(file_record, volume, record, header_damaged, stats,
                       entry)) {
      entries.pop_back();
      continue;
    }
    if (annotate) {
      resolver.Note(file_record, entries.size() - 1);
    }
  }

  std::vector<size_t> dropped;
  for (const HeldRecords::Waiting& waiting : held.WaitingRecords()) {
    Mft::MftTree::Entry& entry = entries.at(waiting.entry_index);
    // The bytes the first pass parsed: the header parses as it did then.
    if (!file_record.ParseFileRecord(waiting.record,
                                     held.Bytes(waiting.slot)) ||
        !ReadBaseEntry(file_record, volume, waiting.record,
                       waiting.header_damaged, stats, entry)) {
      dropped.push_back(waiting.entry_index);
      continue;
    }
    if (annotate) {
      resolver.Note(file_record, waiting.entry_index);
    }
  }
  if (annotate) {
    resolver.Flush();
  }
  DropEntries(entries, dropped);
}

// Reads the record slots of volume's $MFT, keeps each base record's entry,
// then links the entries into a tree. With include_deleted off, the slots
// $MFT's $BITMAP marks free are not read.
template <Cache::Strategy S>
void Mft::MftTree::Impl::Scan(const NtfsVolume<S>& volume,
                              const Mft::MftTree::ScanOptions& options) {
  const ULONGLONG total = volume.GetRecordsCount();
  stats.slots = total;

  // A freed record is dropped anyway without include_deleted, so its read is
  // saved. With it, every slot is read, bitmap or not.
  const std::optional<std::vector<BYTE>> bitmap =
      volume.GetOptions().include_deleted ? std::nullopt
                                          : ReadMftBitmap(volume, total);
  // An Entry is big: each time the vector grows, it moves them all.
  if (bitmap) {
    entries.reserve(gsl::narrow<size_t>(
        (std::min)(CountUsedSlots(*bitmap, total), max_reserved_entries)));
  }

  // Deleted entries only exist with include_deleted, and the annotation reads
  // the bitmap: without both, the scan reads what it always did.
  const bool annotate =
      volume.GetOptions().include_deleted && options.annotate_data_state;
  ReadSlots(volume, options, bitmap, annotate);
  for (size_t index = 0; index < entries.size(); index++) {
    // index < entries.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    IndexEntry(entries[index].record, index);
  }

  if (options.progress && stats.complete) {
    options.progress(total, total);
  }

  LogScanSummary(annotate);
  Link();
  ApplyJournalIfComplete(volume);
}

// Logs the counters of the scan. The data state counters only mean something
// when the scan annotated the entries.
void Mft::MftTree::Impl::LogScanSummary(bool annotated) const {
  Log::Info(
      "MFT scan: {} slots, {} in use, {} deleted, {} unreadable, {} skipped",
      stats.slots, stats.in_use, stats.deleted, stats.unreadable,
      stats.skipped);
  if (annotated) {
    Log::Info("MFT scan data: {} intact, {} reallocated, {} unknown, {} bad",
              stats.data_intact, stats.data_reallocated, stats.data_unknown,
              stats.data_bad);
  }
  if (stats.carved_names != 0) {
    Log::Info("MFT scan slack: {} names carved", stats.carved_names);
  }
}

// Annotates the linked entries from the volume's change journal, when
// recovering and when the scan ran to its end.
template <Cache::Strategy S>
void Mft::MftTree::Impl::ApplyJournalIfComplete(const NtfsVolume<S>& volume) {
  // A scan that progress() stopped holds only some records: a file would look
  // lost to the journal that is merely not read yet.
  if (!volume.GetOptions().recover_errors || !stats.complete) {
    return;
  }
  const Usn::Journal* journal = volume.GetUsnJournal();
  if (journal == nullptr) {
    return;
  }
  ApplyJournal(*journal, volume.GetOptions().include_deleted);
  Log::Info(
      "MFT scan journal: {} records, {} entries annotated, {} only in the "
      "journal",
      stats.usn_records, stats.usn_annotated, stats.usn_journal_only);
}

// Validates every name's parent reference, files each record under the
// parents its valid names give, then marks what the root reaches.
void Mft::MftTree::Impl::Link() {
  for (Mft::MftTree::Entry& entry : entries) {
    for (Mft::MftTree::Name& name : entry.names) {
      name.parent_valid = IsValidParent(entry, name);
    }
  }
  FileChildren();

  reachable.assign(entries.size(), false);
  if (const std::optional<size_t> root = EntryIndex(root_record)) {
    // EntryIndex() gives an index of entries; reachable is as long.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    reachable[*root] = true;
  }

  std::vector<ULONGLONG> pending{root_record};
  while (!pending.empty()) {
    const ULONGLONG dir = pending.back();
    pending.pop_back();
    for (const ULONGLONG child : Children(dir)) {
      // Every child is an entry.
      const size_t index = EntryIndex(child).value();
      // EntryIndex() gives an index of entries; reachable is as long.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      auto flag = reachable[index];
      if (!flag) {
        flag = true;
        pending.push_back(child);
      }
    }
  }

  stats.unreachable = static_cast<ULONGLONG>(
      std::count(reachable.begin(), reachable.end(), false));
}

// Counts the children of each entry, then lays them out in children_records,
// each entry's run in the order of entries.
void Mft::MftTree::Impl::FileChildren() {
  children_first.assign(entries.size() + 1, 0);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = EntryIndex(parent)) {
        // EntryIndex() gives an index below entries.size().
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_first[*index + 1]++;
      } else {
        // Only the root is a valid parent without an entry.
        lost_root_children.push_back(entry.record);
      }
    }
  }
  std::partial_sum(children_first.begin(), children_first.end(),
                   children_first.begin());

  children_records.resize(children_first.back());
  std::vector<size_t> next(children_first.begin(), children_first.end() - 1);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = EntryIndex(parent)) {
        // next is as long as entries, and its values index children_records,
        // which the counts above sized.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_records[next[*index]++] = entry.record;
      }
    }
  }
}

void Mft::MftTree::Impl::IndexEntry(ULONGLONG record, size_t index) {
  const ULONGLONG dense_limit =
      (std::max)(static_cast<ULONGLONG>(dense_index_floor),
                 static_cast<ULONGLONG>(dense_index_ratio) * entries.size());
  if (record >= dense_limit) {
    sparse_entries.emplace(record, index);
    return;
  }
  // record < dense_limit, which is a size_t.
  const auto slot = static_cast<size_t>(record);
  if (slot >= entry_of_record.size()) {
    entry_of_record.resize(slot + 1, no_entry);
  }
  // The resize above makes slot an index of entry_of_record.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  entry_of_record[slot] = index;
}

std::optional<size_t> Mft::MftTree::Impl::EntryIndex(ULONGLONG record) const {
  if (record < entry_of_record.size()) {
    // record < entry_of_record.size() was checked above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t index = entry_of_record[static_cast<size_t>(record)];
    if (index != no_entry) {
      return index;
    }
  }
  if (sparse_entries.empty()) {
    return std::nullopt;
  }
  const auto found = sparse_entries.find(record);
  if (found == sparse_entries.end()) {
    return std::nullopt;
  }
  return found->second;
}

// Whether name's parent reference, read from child's record, names a
// directory the scan found. See the rules in mft/tree.h.
bool Mft::MftTree::Impl::IsValidParent(const Mft::MftTree::Entry& child,
                                       const Mft::MftTree::Name& name) const {
  // Only the root directory is filed under itself.
  if (name.parent_record == child.record) {
    return child.record == root_record;
  }

  return NamesDirectory(name.parent_record, name.parent_sequence);
}

bool Mft::MftTree::Impl::NamesDirectory(ULONGLONG parent_record,
                                        WORD parent_sequence) const {
  const Mft::MftTree::Entry* parent = Find(parent_record);
  if (parent == nullptr) {
    return parent_record == root_record;
  }
  if (!parent->directory) {
    return false;
  }

  // The parent is the same one the reference was written for: unchecked, still
  // the same generation, or the generation NTFS freed right after.
  return Mft::IsSameRecordGeneration(parent_sequence, parent->sequence,
                                     parent->in_use);
}

namespace {

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

// FILE_ATTRIBUTE_DIRECTORY, in the attributes a journal record carries.
constexpr DWORD journal_directory_attribute = 0x10;

}  // namespace

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

// Joins name and its ancestors' primary names up to the root, or up to the
// record where the chain of valid parent references breaks. A file's path is
// its parent's, walked once for all its files, plus its name: a file is never
// a valid parent, so it cannot sit on the chain above it.
std::wstring Mft::MftTree::Impl::PathThrough(
    const Mft::MftTree::Entry& entry, const Mft::MftTree::Name& name,
    std::optional<ULONGLONG>* lost_ancestor) const {
  if (lost_ancestor != nullptr) {
    lost_ancestor->reset();
  }
  if (entry.record == root_record) {
    return L"\\";
  }

  // A valid parent other than the root is a directory the scan found.
  const Mft::MftTree::Entry* parent =
      name.parent_valid && name.parent_record != root_record
          ? Find(name.parent_record)
          : nullptr;
  const Mft::MftTree::Name* parent_name =
      parent != nullptr ? PrimaryName(*parent) : nullptr;

  std::wstring path;
  std::optional<ULONGLONG> lost;
  if (entry.directory && &name == PrimaryName(entry)) {
    // The path its files reuse.
    const DirectoryPath& own = PathOfDirectory(entry, name);
    path = own.path;
    lost = own.lost;
  } else if (!entry.directory && parent_name != nullptr) {
    const DirectoryPath& directory = PathOfDirectory(*parent, *parent_name);
    path.reserve(directory.path.size() + 1 + name.name.size());
    path = directory.path;
    path += L'\\';
    path += name.name;
    lost = directory.lost;
  } else {
    path = WalkPath(entry, name, lost);
  }

  if (lost_ancestor != nullptr) {
    *lost_ancestor = lost;
  }
  return path;
}

// The path of directory through primary, its primary name: walked the first
// time, then remembered.
const Mft::MftTree::Impl::DirectoryPath& Mft::MftTree::Impl::PathOfDirectory(
    const Mft::MftTree::Entry& directory,
    const Mft::MftTree::Name& primary) const {
  if (const auto found = directory_paths_.find(directory.record);
      found != directory_paths_.end()) {
    return found->second;
  }
  DirectoryPath walked;
  walked.path = WalkPath(directory, primary, walked.lost);
  return directory_paths_.emplace(directory.record, std::move(walked))
      .first->second;
}

// Walks from name up the primary names of entry's ancestors, see
// PathThrough(). entry is not the root. lost receives the record where the
// chain breaks, if it does.
std::wstring
    Mft::MftTree::Impl::WalkPath(const Mft::MftTree::Entry& entry,
                                 const Mft::MftTree::Name& name,
                                 std::optional<ULONGLONG>& lost) const {
  std::vector<const std::wstring*> parts;
  // Guards against a forged chain of parents that loops back on itself.
  std::unordered_set<ULONGLONG> visited{entry.record};
  const Mft::MftTree::Name* current = &name;
  lost.reset();

  while (true) {
    parts.push_back(&current->name);
    const ULONGLONG parent = current->parent_record;
    if (!current->parent_valid || !visited.insert(parent).second) {
      lost = parent;
      break;
    }
    if (parent == root_record) {
      break;
    }

    // parent_valid comes from IsValidParent(), which finds parent in the scan.
    const Mft::MftTree::Entry* parent_entry = Find(parent);
    assert(parent_entry != nullptr);
    current = PrimaryName(*parent_entry);
    if (current == nullptr) {
      lost = parent;
      break;
    }
  }

  std::wstring path;
  for (const auto* const part : parts | std::views::reverse) {
    if (!path.empty() || !lost) {
      path += L'\\';
    }
    path += *part;
  }
  return path;
}

const std::vector<Mft::MftTree::Entry>& Mft::MftTree::Entries() const noexcept {
  return impl_->entries;
}

// The lookup behind MftTree::Find().
const Mft::MftTree::Entry* Mft::MftTree::Impl::Find(ULONGLONG record) const {
  const std::optional<size_t> index = EntryIndex(record);
  // EntryIndex() gives an index of entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return index ? &entries[*index] : nullptr;
}

// The lookup behind MftTree::Children().
std::span<const ULONGLONG>
    Mft::MftTree::Impl::Children(ULONGLONG dir_record) const {
  const std::optional<size_t> index = EntryIndex(dir_record);
  if (!index) {
    return dir_record == root_record
               ? std::span<const ULONGLONG>{lost_root_children}
               : std::span<const ULONGLONG>{};
  }
  // children_first holds one more value than there are entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t first = children_first[*index];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t last = children_first[*index + 1];
  return std::span<const ULONGLONG>{children_records}.subspan(first,
                                                              last - first);
}

const Mft::MftTree::Entry* Mft::MftTree::Find(ULONGLONG record) const {
  return impl_->Find(record);
}

std::span<const ULONGLONG> Mft::MftTree::Children(ULONGLONG dir_record) const {
  return impl_->Children(dir_record);
}

bool Mft::MftTree::IsReachable(ULONGLONG record) const {
  const std::optional<size_t> index = impl_->EntryIndex(record);
  if (!index) {
    return false;
  }
  // EntryIndex() gives an index of entries; reachable is as long.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->reachable[*index];
}

std::wstring
    Mft::MftTree::GetPath(ULONGLONG record,
                          std::optional<ULONGLONG>* lost_ancestor) const {
  const Mft::MftTree::Entry* entry = impl_->Find(record);
  const Mft::MftTree::Name* name =
      entry != nullptr ? PrimaryName(*entry) : nullptr;
  if (name == nullptr) {
    if (lost_ancestor != nullptr) {
      lost_ancestor->reset();
    }
    return {};
  }
  return impl_->PathThrough(*entry, *name, lost_ancestor);
}

std::wstring
    Mft::MftTree::GetPath(ULONGLONG record, size_t name_index,
                          std::optional<ULONGLONG>* lost_ancestor) const {
  const Mft::MftTree::Entry* entry = impl_->Find(record);
  if (entry == nullptr || name_index >= entry->names.size()) {
    if (lost_ancestor != nullptr) {
      lost_ancestor->reset();
    }
    return {};
  }
  // nameIndex < names.size() was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->PathThrough(*entry, entry->names[name_index], lost_ancestor);
}

std::span<const Mft::MftTree::JournalOnlyEntry>
    Mft::MftTree::JournalOnly() const noexcept {
  return impl_->journal_only;
}

std::wstring Mft::MftTree::GetJournalPath(
    size_t index, std::optional<ULONGLONG>* lost_ancestor) const {
  if (lost_ancestor != nullptr) {
    lost_ancestor->reset();
  }
  if (index >= impl_->journal_only.size()) {
    return {};
  }
  // index is checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const Mft::MftTree::JournalOnlyEntry& item = impl_->journal_only[index];

  if (!item.parent_valid) {
    if (lost_ancestor != nullptr) {
      *lost_ancestor = item.parent_record;
    }
    return item.name;
  }
  // The root is always a valid parent, even when its own record is lost.
  if (item.parent_record == root_record) {
    return L"\\" + item.name;
  }

  std::wstring path = GetPath(item.parent_record, lost_ancestor);
  if (path.empty()) {
    if (lost_ancestor != nullptr) {
      *lost_ancestor = item.parent_record;
    }
    return item.name;
  }
  if (path.back() != L'\\') {
    path += L'\\';
  }
  return path + item.name;
}

const Mft::MftTree::ScanStats& Mft::MftTree::Stats() const noexcept {
  return impl_->stats;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
