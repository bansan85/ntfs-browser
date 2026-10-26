#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <thread>
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
#include "util/chunk-pool.h"

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
// base record is read again in the second pass, and so are the extension
// records that no longer fit: the pass announces them to the reader ahead of
// its reads. A system volume of 5 million slots needs about 735 000 held
// records, 718 MiB: 64 bits keep 1 GiB, 32 bits keep most of their address
// space free.
constexpr size_t default_max_held_record_bytes =
    sizeof(void*) >= 8 ? size_t{1} << 30 : size_t{128} << 20;

// The cap a test asked for. nullopt means the default.
std::optional<size_t> max_held_record_bytes_override;

// Slots the scans of this thread took from the threads that parsed them. Only
// the thread that owns a scan changes it, and tests are the only readers.
thread_local size_t slots_parsed_by_threads = 0;

// Bytes of one block of held records. Records are held in blocks, so holding
// one more never moves those already held.
constexpr size_t held_block_bytes = size_t{4} << 20;

// Waiting records whose reads the second pass announces to the reader ahead of
// the one it parses. The reader reads up to 8 ranges and 16 MiB at once: 64
// records keep it busy, and the $ATTRIBUTE_LIST streams of 64 of them are 16
// MiB at most.
constexpr size_t max_announced_records = 64;

// Ranges the second pass has announced and not read, at most. The reader keeps
// 1024 and drops a hint past them, and then no read of it is served: half of
// that leaves room for the ranges of one record, and the reader looks for the
// range of a read among the first 64 only, so an announcement is read at once.
constexpr size_t max_announced_ranges = 512;

// Waiting records between two restarts of the announcements. A range that no
// read uses stays at the front of the reader's list and blocks the ones behind
// it, until a read of a later one passes it. A restart bounds the damage of one
// the pass could not foresee.
constexpr size_t announce_restart_interval = 1024;

// Extension records the second pass reads at once for one waiting base record,
// at most: 4096 are 4 MiB. The others are read from the disk, one at a time,
// when the record asks for them.
constexpr size_t max_preread_extensions = 4096;

// Records one read of the second pass covers at most, and one announced range:
// 64 are 64 KiB at the usual 1 KiB per record.
constexpr size_t max_run_slots = 64;

// Slots of $MFT whose base record number the first pass remembers at most. The
// numbers take 4 bytes each: 2^26 slots, 64 GiB of $MFT at 1 KiB per record,
// are 256 MiB. A slot past it keeps its disk read.
constexpr ULONGLONG max_remembered_slots = ULONGLONG{1} << 26;

// Slots the first pass reserves room for at most, before it knows how many it
// meets: 2^24 are 64 MiB, and cover 16 GiB of $MFT.
constexpr ULONGLONG max_reserved_slots = ULONGLONG{1} << 24;

// What SlotBases keeps for a slot the first pass did not read, or whose base is
// too large for 4 bytes.
constexpr std::uint32_t unknown_base =
    (std::numeric_limits<std::uint32_t>::max)();

// Bytes of one $ATTRIBUTE_LIST announced at most: the largest Windows writes.
// A forged size announces no more.
constexpr ULONGLONG max_announced_list_bytes = ULONGLONG{256} * 1024;

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

// Slots one chunk of a parsed window holds. A thread takes a chunk at a time:
// 64 records are about 250 microseconds of work, long enough that taking the
// next chunk costs nothing, short enough that the threads finish together.
constexpr size_t parse_chunk_slots = 64;

// Slots below which an automatic thread count stays at one thread: twice the
// probe, so that threads have as much to parse as the probe did.
constexpr ULONGLONG default_min_auto_threaded_slots = 131072;

// Slots an automatic scan parses on the calling thread alone before it decides
// whether threads pay: 16 windows of the usual 4 MiB, about 0.2 seconds of a
// system volume, long enough for the share of waiting to show.
constexpr ULONGLONG default_probe_slots = 65536;

// Most of the time of the probe the loading of windows may take for threads to
// pay. A volume read faster than one thread parses gives windows at once: the
// load is a copy, about a tenth of the time of a window. A volume read slower
// makes the scan wait, and a SATA disk takes more than a third.
constexpr double max_load_share = 0.2;

// The limits a test asked for, and the verdict it imposed on the probe. nullopt
// means the default, and the measure.
std::optional<ULONGLONG> min_auto_threaded_slots_override;
std::optional<ULONGLONG> probe_slots_override;
std::optional<bool> probe_verdict_override;

// Waiting base records one batch of the second pass holds. The threads parse a
// batch while the scan's thread merges the one before it in order: 4096
// outcomes are a few MiB, and a batch is long enough that starting it costs
// nothing.
constexpr size_t default_waiting_batch_records = 4096;

// The batch length a test asked for. nullopt means the default.
std::optional<size_t> waiting_batch_records_override;

// Waiting base records one chunk of a batch holds. A held record takes about 4
// microseconds: 64 are about 250 microseconds, long enough that taking the
// next chunk costs nothing, short enough that the threads finish together.
constexpr size_t waiting_chunk_records = 64;

// Waiting base records below which an automatic thread count keeps the second
// pass on one thread: 2048 held records take about 8 milliseconds, little
// more than starting the threads costs.
constexpr size_t default_min_auto_threaded_waiting = 2048;

// The limit a test asked for. nullopt means the default.
std::optional<size_t> min_auto_threaded_waiting_override;

// Waiting records the scans of this thread took from the threads that parsed
// them, in the second pass. Read by tests only, like slots_parsed_by_threads.
thread_local size_t waiting_parsed_by_threads = 0;

// Threads an automatic count takes at most. The calling thread merges the
// results in record order, and that work does not shrink with more threads:
// past four, a scan of the whole $MFT of a system volume gains nothing, and the
// threads that share a core only add CPU time.
constexpr unsigned max_auto_threads = 4;

// Threads a caller can ask for at most. A forged count cannot start more.
constexpr unsigned max_scan_threads = 64;

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

// The counters one record slot adds to the scan's statistics. A slot parsed
// away from the thread that merges the scan keeps them apart, and they join the
// statistics when the scan reaches the slot: a slot that is parsed twice is
// counted once.
struct RecordCounts {
  ULONGLONG unreadable{0};
  ULONGLONG damaged{0};
  ULONGLONG extensions{0};
  ULONGLONG deleted{0};
  ULONGLONG in_use{0};
  ULONGLONG reused_extensions{0};
  ULONGLONG carved_names{0};
};

// Adds counts to stats.
void AddCounts(Mft::MftTree::ScanStats& stats,
               const RecordCounts& counts) noexcept {
  stats.unreadable += counts.unreadable;
  stats.damaged += counts.damaged;
  stats.extensions += counts.extensions;
  stats.deleted += counts.deleted;
  stats.in_use += counts.in_use;
  stats.reused_extensions += counts.reused_extensions;
  stats.carved_names += counts.carved_names;
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

// The base record each slot of $MFT named when the first pass read it: 0 for a
// base record, the base's record number for an extension. A record an
// $ATTRIBUTE_LIST names, that is not held, and whose slot names another base
// than the list's own, is not an extension of it: the second pass skips it
// without reading it again.
class SlotBases {
 public:
  explicit SlotBases(ULONGLONG total) {
    bases_.reserve(gsl::narrow<size_t>((std::min)(total, max_reserved_slots)));
  }

  // Remembers that slot record names base. Slots below Mft::Idx::User are
  // not remembered: the disk reads them, with the $MFTMirr fallback.
  void Note(ULONGLONG record, ULONGLONG base) {
    if (record < static_cast<ULONGLONG>(Mft::Idx::User) ||
        record >= max_remembered_slots) {
      return;
    }
    const auto slot = gsl::narrow<size_t>(record);
    if (bases_.size() <= slot) {
      bases_.resize(slot + 1, unknown_base);
    }
    // resize() above covers slot.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bases_[slot] =
        base < unknown_base ? static_cast<std::uint32_t>(base) : unknown_base;
  }

  // The base record number slot record named, or nothing when it is unknown.
  [[nodiscard]] std::optional<ULONGLONG> BaseOf(ULONGLONG record) const {
    if (record >= bases_.size()) {
      return std::nullopt;
    }
    // record < size() was just checked.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const std::uint32_t base = bases_[gsl::narrow<size_t>(record)];
    if (base == unknown_base) {
      return std::nullopt;
    }
    return base;
  }

 private:
  std::vector<std::uint32_t> bases_;
};

// Raw $MFT records a scan holds for its second pass, as the slot reader read
// them: the extension records that the waiting base records will ask for, and
// those base records. Both kinds share one budget, MaxHeldRecordBytes(). A
// record that does not fit is not held: a base record waits as its number, an
// extension record is listed with its base, and the second pass announces both
// to the reader.
//
// An extension record is of use to its base record only. A base met before it
// is known to wait or not. A base met after it is not yet: the extension is
// held provisionally, and released once the base turns out not to wait.
class HeldRecords {
 public:
  // A base record that waits for the second pass, with the entry kept in its
  // place, and what its first header read counted.
  struct Waiting {
    ULONGLONG record;
    size_t entry_index;
    bool header_damaged;
    // Where its bytes are held. Nothing when the budget was spent: the second
    // pass reads the record again.
    std::optional<size_t> slot;
    // The clusters its $ATTRIBUTE_LIST is read from, if it is not resident.
    std::vector<Alloc::Extent> list_extents;
    // Its extension records no slot holds, in increasing order: the second
    // pass reads them from the disk.
    std::vector<ULONGLONG> unheld_extensions;
  };

  explicit HeldRecords(size_t record_size)
      : record_size_(record_size),
        block_slots_(record_size != 0
                         ? (std::max)(size_t{1}, held_block_bytes / record_size)
                         : 1),
        budget_(MaxHeldRecordBytes()) {}

  // Meets extension record, whose base is base_record, with the bytes the slot
  // reader read. Records MUST come in increasing order.
  void MeetExtension(ULONGLONG record, ULONGLONG base_record,
                     std::span<const BYTE> bytes) {
    if (base_record < record) {
      if (Waiting* const waiting = FindWaiting(base_record)) {
        Keep({.record = record, .slot = {}}, bytes, *waiting);
      }
      return;
    }
    // An extension that names itself has no base to ask for it.
    if (base_record == record) {
      return;
    }

    Provisional added{.record = record, .slot = {}};
    size_t slot = 0;
    if (Store(bytes, slot)) {
      added.slot = slot;
    }
    pending_[base_record].push_back(added);
  }

  // Holds base record for the second pass, with the extension records met
  // before it that it will ask for. False when bytes is not a record, which
  // the first pass then parses at once. When the budget is spent, the record
  // waits without its bytes.
  bool HoldWaiting(ULONGLONG record, std::span<const BYTE> bytes,
                   size_t entry_index, bool header_damaged,
                   std::vector<Alloc::Extent> list_extents) {
    if (bytes.size() != record_size_) {
      return false;
    }
    Waiting added{.record = record,
                  .entry_index = entry_index,
                  .header_damaged = header_damaged,
                  .slot = {},
                  .list_extents = std::move(list_extents),
                  .unheld_extensions = {}};
    size_t slot = 0;
    if (Store(bytes, slot)) {
      added.slot = slot;
    }
    for (const Provisional& extension : TakePending(record)) {
      Keep(extension, {}, added);
    }
    waiting_.push_back(std::move(added));
    return true;
  }

  // Keeps the held extension records met before base_record, which the first
  // pass parses at once: it asks for them in the middle of the scan.
  void Confirm(ULONGLONG base_record) {
    for (const Provisional& extension : TakePending(base_record)) {
      if (extension.slot) {
        extensions_.emplace(extension.record, *extension.slot);
      }
    }
  }

  // Releases the extension records met before a base record that precedes
  // record and did not wait: nothing asks for them.
  void ReleaseBefore(ULONGLONG record) {
    while (!pending_.empty() && pending_.begin()->first < record) {
      Release(pending_.begin()->second);
      pending_.erase(pending_.begin());
    }
  }

  // Ends the first pass: no base record is left to come.
  void Finish() {
    for (const auto& [base, extensions] : pending_) {
      Release(extensions);
    }
    pending_.clear();
  }

  // The bytes of extension record, or an empty span when it is not held.
  [[nodiscard]] std::span<const BYTE> FindExtension(ULONGLONG record) const {
    const auto found = extensions_.find(record);
    if (found == extensions_.end()) {
      return {};
    }
    return Bytes(found->second);
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
  // An extension record met before its base record: held, or not for lack of
  // room.
  struct Provisional {
    ULONGLONG record;
    std::optional<size_t> slot;
  };

  // The waiting base record record, or null.
  [[nodiscard]] Waiting* FindWaiting(ULONGLONG record) {
    const auto found =
        std::ranges::lower_bound(waiting_, record, {}, &Waiting::record);
    return found != waiting_.end() && found->record == record ? &*found
                                                              : nullptr;
  }

  // Gives waiting the extension record it asks for: from the slot it was held
  // in, or from the disk if it was not. bytes is that record's, when it is not
  // held yet.
  void Keep(const Provisional& extension, std::span<const BYTE> bytes,
            Waiting& waiting) {
    std::optional<size_t> slot = extension.slot;
    if (!slot) {
      size_t stored = 0;
      if (Store(bytes, stored)) {
        slot = stored;
      }
    }
    if (slot) {
      extensions_.emplace(extension.record, *slot);
    } else {
      waiting.unheld_extensions.push_back(extension.record);
    }
  }

  // Removes and returns what waits for base_record.
  std::vector<Provisional> TakePending(ULONGLONG base_record) {
    const auto found = pending_.find(base_record);
    if (found == pending_.end()) {
      return {};
    }
    std::vector<Provisional> taken = std::move(found->second);
    pending_.erase(found);
    return taken;
  }

  // Frees the slots of extensions for the next records to reuse.
  void Release(const std::vector<Provisional>& extensions) {
    for (const Provisional& extension : extensions) {
      if (extension.slot) {
        free_slots_.push_back(*extension.slot);
        live_--;
      }
    }
  }

  // Copies bytes, one record, into a free slot, or the next one. False when
  // bytes is not a record or the budget is spent.
  bool Store(std::span<const BYTE> bytes, size_t& slot) {
    if (bytes.size() != record_size_) {
      return false;
    }
    if (!free_slots_.empty()) {
      slot = free_slots_.back();
      free_slots_.pop_back();
      std::vector<BYTE>& block = blocks_.at(slot / block_slots_);
      const size_t offset = (slot % block_slots_) * record_size_;
      std::ranges::copy(
          bytes, std::next(block.begin(), gsl::narrow<std::ptrdiff_t>(offset)));
      live_++;
      return true;
    }
    if ((live_ + 1) * record_size_ > budget_) {
      return false;
    }
    if (allocated_ % block_slots_ == 0) {
      blocks_.emplace_back().reserve(block_slots_ * record_size_);
    }
    std::vector<BYTE>& block = blocks_.back();
    block.insert(block.end(), bytes.begin(), bytes.end());
    slot = allocated_++;
    live_++;
    return true;
  }

  size_t record_size_;
  // Records in one block.
  size_t block_slots_;
  // Bytes this scan may hold.
  size_t budget_;
  // Slots given out so far, free ones included, and slots in use among them.
  size_t allocated_{0};
  size_t live_{0};
  // Slots Release() gave back.
  std::vector<size_t> free_slots_;
  std::vector<std::vector<BYTE>> blocks_;
  // The held extension records, by record, and their slot.
  std::unordered_map<ULONGLONG, size_t> extensions_;
  // The extension records met before their base record, by that base.
  std::map<ULONGLONG, std::vector<Provisional>> pending_;
  // Sorted by record: the scan meets them in that order.
  std::vector<Waiting> waiting_;
};

// The extension records the second pass reads for one waiting base record, that
// the first pass could not hold. It reads them before it parses the record, in
// increasing order and run by run: that is the order the reader was told, so it
// serves them from what it read ahead. A record the list names several times
// is read once. Holds at most max_preread_extensions records.
template <Cache::Strategy S>
class UnheldExtensions {
 public:
  explicit UnheldExtensions(const NtfsVolume<S>& volume)
      : volume_(&volume), record_size_(volume.GetFileRecordSize()) {}

  // Reads the records of the first max_preread_extensions of records, in
  // increasing order. A run that cannot be read is left out: the record is
  // then read from the disk when asked for, as when it is not here.
  void Load(const std::vector<ULONGLONG>& records) {
    Clear();
    const size_t count = (std::min)(records.size(), max_preread_extensions);
    for (size_t first = 0; first < count;) {
      size_t run = 1;
      // The run goes on while the next record directly follows, up to its cap.
      while (first + run < count && run < max_run_slots &&
             records.at(first + run) == records.at(first) + run) {
        run++;
      }
      Read(records.at(first), run);
      first += run;
    }
  }

  // The bytes of record, or an empty span when it was not loaded.
  [[nodiscard]] std::span<const BYTE> Find(ULONGLONG record) const {
    const auto found = offsets_.find(record);
    if (found == offsets_.end()) {
      return {};
    }
    return std::span<const BYTE>{bytes_}.subspan(found->second, record_size_);
  }

  // Forgets every record.
  void Clear() noexcept {
    bytes_.clear();
    offsets_.clear();
  }

 private:
  // Reads the run records from first on, and remembers them.
  void Read(ULONGLONG first, size_t run) {
    const size_t start = bytes_.size();
    bytes_.resize(start + run * record_size_);
    const std::span<BYTE> out =
        std::span<BYTE>{bytes_}.subspan(start, run * record_size_);
    if (!volume_->ReadMftSlots(first, out)) {
      bytes_.resize(start);
      return;
    }
    for (size_t i = 0; i < run; i++) {
      offsets_.emplace(first + i, start + i * record_size_);
    }
  }

  const NtfsVolume<S>* volume_;
  size_t record_size_;
  std::vector<BYTE> bytes_;
  std::unordered_map<ULONGLONG, size_t> offsets_;
};

// Announces to the volume's reader what the second pass reads from the disk, a
// number of waiting records ahead of it and in the order it reads it: the
// record when it was not held, its extension records that were not held, and
// its $ATTRIBUTE_LIST stream. The disk reads them while the pass parses.
//
// The reader serves a read only from the first announced ranges, and drops
// those before the one it serves. A range no read uses, because the pass does
// not read what was announced, would block every later one. Restart() clears
// them.
template <Cache::Strategy S>
class ReadAnnouncer {
 public:
  ReadAnnouncer(const NtfsVolume<S>& volume,
                const std::vector<HeldRecords::Waiting>& waiting)
      : volume_(&volume), waiting_(&waiting), ranges_(waiting.size(), 0) {}

  // Called before the pass parses waiting[index]. Announces the records that
  // follow those already announced, until max_announced_records of them, or
  // max_announced_ranges ranges, wait from index on.
  void Before(size_t index) {
    for (; passed_ < index; passed_++) {
      if (ranges_.at(passed_) != 0) {
        ahead_records_--;
        ahead_ranges_ -= ranges_.at(passed_);
      }
    }
    // Another record's reads fit: neither cap is reached.
    for (; next_ < waiting_->size() && ahead_records_ < max_announced_records &&
           ahead_ranges_ < max_announced_ranges;
         next_++) {
      const HeldRecords::Waiting& waiting = waiting_->at(next_);
      if (!HasReads(waiting)) {
        continue;
      }
      if (!cancelled_) {
        // A first pass that stopped early leaves blocks announced, which
        // would be read for nothing, and before these.
        volume_->CancelPrefetch();
        cancelled_ = true;
      }
      const size_t announced = Announce(waiting);
      ranges_.at(next_) = announced;
      ahead_records_++;
      ahead_ranges_ += announced;
    }
  }

  // Withdraws everything announced and starts again at waiting[index]: the
  // pass did not read what it announced for the record before.
  void Restart(size_t index) {
    volume_->CancelPrefetch();
    std::ranges::fill(ranges_, size_t{0});
    passed_ = index;
    next_ = index;
    ahead_records_ = 0;
    ahead_ranges_ = 0;
  }

 private:
  // Whether the pass reads anything for waiting from the disk.
  [[nodiscard]] static bool HasReads(const HeldRecords::Waiting& waiting) {
    return !waiting.slot || !waiting.list_extents.empty() ||
           !waiting.unheld_extensions.empty();
  }

  // Announces the reads of waiting, in the order the pass makes them, and
  // returns how many ranges it announced.
  size_t Announce(const HeldRecords::Waiting& waiting) const {
    size_t announced = 0;
    if (!waiting.slot) {
      volume_->PrefetchMftSlots(waiting.record, 1);
      announced++;
    }
    // Neighbouring records are one read.
    const std::vector<ULONGLONG>& unheld = waiting.unheld_extensions;
    const size_t count = (std::min)(unheld.size(), max_preread_extensions);
    for (size_t first = 0; first < count;) {
      size_t run = 1;
      // As in UnheldExtensions::Load(): the same runs, so the same reads.
      while (first + run < count && run < max_run_slots &&
             unheld.at(first + run) == unheld.at(first) + run) {
        run++;
      }
      volume_->PrefetchMftSlots(unheld.at(first), run);
      announced++;
      first += run;
    }
    const ULONGLONG cluster_size = volume_->GetClusterSize();
    for (const Alloc::Extent& extent : waiting.list_extents) {
      volume_->Prefetch(extent.lcn * cluster_size,
                        extent.clusters * cluster_size);
      announced++;
    }
    return announced;
  }

  const NtfsVolume<S>* volume_;
  const std::vector<HeldRecords::Waiting>* waiting_;
  // The ranges announced for each record, 0 when none or not announced yet.
  std::vector<size_t> ranges_;
  // Records the pass parsed.
  size_t passed_{0};
  // The first record whose reads are not announced.
  size_t next_{0};
  // Records with reads, and their ranges, announced among those from passed_
  // to next_.
  size_t ahead_records_{0};
  size_t ahead_ranges_{0};
  bool cancelled_{false};
};

// What one record slot holds, once its header is read.
enum class SlotKind : BYTE {
  Unreadable,  // Its header does not parse.
  Dropped,     // Freed and not asked for.
  Extension,   // An extension record: its base record imports its attributes.
  Base         // A base record, kept if its attributes allow it.
};

// Classifies the record slot file_record holds, and counts it in counts.
// parsed is the verdict of the header parse. A record with a failed update
// sequence check is damaged in both modes: strict drops it as unreadable,
// recovering keeps what verifies. It is counted once, whatever else is wrong
// with it: header_damaged tells ReadBaseEntry() it is counted already.
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

// Reads the base record file_record holds into entry, fresh. Returns false for
// a record not kept, a damaged one when strict: entry is then meaningless.
template <Cache::Strategy S>
bool ReadBaseEntry(Io::FileRecord<S>& file_record, const NtfsVolume<S>& volume,
                   ULONGLONG record, bool header_damaged, RecordCounts& counts,
                   Mft::MftTree::Entry& entry) {
  const bool parsed = ReadEntry(file_record, record, entry);
  counts.reused_extensions += file_record.GetReusedExtensionCount();
  if (!parsed || file_record.IsSalvaged()) {
    if (!header_damaged) {
      counts.damaged++;
    }
    // Strict: a damaged record is dropped, not kept partial.
    if (!parsed && !volume.GetOptions().recover_errors) {
      return false;
    }
  }
  AddCarvedNames(file_record, entry, counts);
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

// Works out the DataNote of the unnamed $DATA stream of file_record.
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

  // Handles the entry at entry_index, whose record gave note.
  void Add(size_t entry_index, DataNote note) {
    if (note.kind == DataNote::Kind::None) {
      return;
    }
    if (note.kind == DataNote::Kind::Known) {
      Set(entry_index, note.state);
      return;
    }

    waiting_.push_back({.entry_index = entry_index,
                        .first_query = queries_.size(),
                        .query_count = note.extents.extents.size(),
                        .complete = note.extents.complete});
    for (const Alloc::Extent& extent : note.extents.extents) {
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

// What one record slot of a window gave, parsed away from the thread that owns
// the scan and kept until the scan reaches the slot.
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
  // The lines the parse logged, in the capture of the chunk it ran in, and then
  // the lines its attributes logged when they were dropped. With one thread,
  // the second kind comes later: when the next slot is parsed.
  Log::Detail::CapturedLog::Mark log_begin;
  Log::Detail::CapturedLog::Mark log_end;
  Log::Detail::CapturedLog::Mark release_end;
};

// Parses record, whose bytes are one slot of a window, into outcome. It runs on
// any thread: it never reads the volume, and it logs into the capture of the
// thread. A record that needs the volume is only marked, for the thread that
// owns the scan to parse: the key stream of an encrypted file may be
// non-resident, and the extension records of an $ATTRIBUTE_LIST are read from
// the disk. file_record MUST NOT resolve an $ATTRIBUTE_LIST.
template <Cache::Strategy S>
void ParseWindowSlot(Io::FileRecord<S>& file_record,
                     const NtfsVolume<S>& volume, ULONGLONG record,
                     std::span<const BYTE> bytes, bool annotate,
                     SlotOutcome& outcome) {
  const bool parsed = file_record.ParseFileRecord(record, bytes);
  outcome.kind = ClassifySlot(file_record, volume, parsed, outcome.counts,
                              outcome.header_damaged);
  if (outcome.kind == SlotKind::Unreadable) {
    return;
  }
  outcome.base_reference = file_record.GetBaseRecordReference();
  if (outcome.kind != SlotKind::Base) {
    return;
  }

  outcome.has_list = file_record.HasAttrHeader(Attr::Type::AttributeList);
  if (outcome.has_list) {
    outcome.list_extents =
        file_record.AttributeListExtents(max_announced_list_bytes);
    return;
  }
  if (file_record.HasAttrHeader(Attr::Type::LoggedUtilityStream)) {
    outcome.needs_scan_thread = true;
    return;
  }

  outcome.kept =
      ReadBaseEntry(file_record, volume, record, outcome.header_damaged,
                    outcome.counts, outcome.entry);
  if (outcome.kept && annotate) {
    outcome.note = ComputeDataNote(file_record);
  }
}

// Parses the slots of one window on several threads, and gives the outcome of
// each in record order. The window is cut into chunks. The threads take them in
// order, each with a FileRecord of its own, and a chunk logs into a capture of
// its own, which the thread that owns the scan replays record by record.
template <Cache::Strategy S>
class WindowParser {
 public:
  // threads is the number of threads that parse, the calling one included. A
  // null bitmap parses every slot, else the slots it marks free are left alone.
  WindowParser(const NtfsVolume<S>& volume, size_t threads,
               const std::vector<BYTE>* bitmap, bool annotate)
      : volume_(&volume),
        record_size_(volume.GetFileRecordSize()),
        bitmap_(bitmap),
        annotate_(annotate),
        pool_(threads - 1) {
    contexts_.reserve(pool_.Slots());
    for (size_t slot = 0; slot < pool_.Slots(); slot++) {
      Io::FileRecord<S>& context =
          *contexts_.emplace_back(std::make_unique<Io::FileRecord<S>>(volume));
      context.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
      // Resolving a list reads the volume, which no worker does. A record with
      // a list never gets to parse its attributes here.
      context.SetResolveAttrList(false);
    }
  }

  // Starts parsing window, after the one before it was ended. The bytes of
  // window MUST stay as they are until End().
  void Begin(const Mft::SlotWindow& window) {
    End();
    window_ = window;
    const auto slots = static_cast<size_t>(window.last - window.first);
    if (outcomes_.size() < slots) {
      outcomes_.resize(slots);
    }
    const size_t chunks = (slots + parse_chunk_slots - 1) / parse_chunk_slots;
    // The logs of the window before stay as they are: the lines its last slot
    // logged when its attributes were dropped are replayed with the next slot.
    log_set_ = 1 - log_set_;
    std::vector<Log::Detail::CapturedLog>& logs = logs_.at(log_set_);
    if (logs.size() < chunks) {
      logs.resize(chunks);
    }
    // Asked here, on the thread that owns the sinks: a worker never does.
    levels_ = Log::Detail::EnabledLevels();
    waited_chunk_ = no_chunk;
    active_ = true;
    pool_.Start(chunks,
                [this](size_t chunk, size_t slot) { ParseChunk(chunk, slot); });
  }

  // Waits for the threads that still parse, and drops the chunks not started.
  void End() {
    if (active_) {
      active_ = false;
      pool_.Finish();
    }
  }

  // Whether the window being parsed holds slot record.
  [[nodiscard]] bool Covers(ULONGLONG record) const noexcept {
    return active_ && record >= window_.first && record < window_.last;
  }

  // The outcome of slot record, once its chunk is parsed. record MUST be a slot
  // of the window that is not marked free. The reference stays valid until the
  // next Begin().
  SlotOutcome& Take(ULONGLONG record) {
    const auto index = static_cast<size_t>(record - window_.first);
    const size_t chunk = index / parse_chunk_slots;
    if (chunk != waited_chunk_) {
      pool_.WaitFor(chunk);
      waited_chunk_ = chunk;
    }
    return outcomes_.at(index);
  }

  // The log that holds the lines the parse of record logged, which the
  // outcome marks. It stays valid until the second Begin() after the window.
  [[nodiscard]] const Log::Detail::CapturedLog& LogOf(ULONGLONG record) const {
    const auto index = static_cast<size_t>(record - window_.first);
    return logs_.at(log_set_).at(index / parse_chunk_slots);
  }

 private:
  // The chunk number of no chunk.
  static constexpr size_t no_chunk = (std::numeric_limits<size_t>::max)();

  // Parses the slots of one chunk on thread slot.
  void ParseChunk(size_t chunk, size_t slot) {
    Log::Detail::CapturedLog& log = logs_.at(log_set_).at(chunk);
    log.Reset(levels_);
    const Log::Detail::CaptureScope capture(log);
    Io::FileRecord<S>& file_record = *contexts_.at(slot);

    const auto slots = static_cast<size_t>(window_.last - window_.first);
    const size_t first = chunk * parse_chunk_slots;
    const size_t last = (std::min)(first + parse_chunk_slots, slots);
    for (size_t index = first; index < last; index++) {
      const ULONGLONG record = window_.first + index;
      if (bitmap_ != nullptr && IsMarkedFree(*bitmap_, record)) {
        continue;
      }
      SlotOutcome& outcome = outcomes_.at(index);
      outcome = SlotOutcome{};
      outcome.log_begin = log.End();
      ParseWindowSlot(file_record, *volume_, record,
                      window_.bytes.subspan(index * record_size_, record_size_),
                      annotate_, outcome);
      outcome.log_end = log.End();
      // What the attributes log when they are dropped is kept apart: it
      // belongs after what the scan does with the slot.
      file_record.ClearAttrs();
      outcome.release_end = log.End();
    }
  }

  const NtfsVolume<S>* volume_;
  size_t record_size_;
  const std::vector<BYTE>* bitmap_;
  bool annotate_;
  Mft::SlotWindow window_;
  Log::Detail::LevelMask levels_{0};
  bool active_{false};
  // The last chunk Take() waited for, so that a chunk is waited for once.
  size_t waited_chunk_{no_chunk};
  std::vector<std::unique_ptr<Io::FileRecord<S>>> contexts_;
  std::vector<SlotOutcome> outcomes_;
  // The capture of each chunk, for the window being parsed and for the one
  // before it.
  std::array<std::vector<Log::Detail::CapturedLog>, 2> logs_;
  size_t log_set_{0};
  // Last: its threads use the members above, so it ends first.
  Util::ChunkPool pool_;
};

// Parses the base records that wait for the second pass, on several threads,
// and gives the outcome of each in waiting order. A batch of waiting records is
// cut into chunks, as WindowParser does with the slots of a window: a thread
// takes a chunk at a time with a FileRecord of its own, and a chunk logs into a
// capture of its own, which the scan's thread replays record by record.
//
// A thread reads nothing of the volume. It parses a record whose bytes are held
// and whose extension records are all held. Its FileRecord refuses a read that
// a parse would make after all: an extension record not held, a non-resident
// $ATTRIBUTE_LIST or $EFS stream. The record is then left to the scan's thread,
// which parses it as the single thread does, reads included. The held records
// and the bases of the slots are not changed during the second pass, so the
// threads only read them.
template <Cache::Strategy S>
class WaitingParser {
 public:
  // threads is the number of threads that parse, the calling one included.
  // held, bases and waiting MUST NOT change while a batch is parsed.
  WaitingParser(const NtfsVolume<S>& volume, size_t threads,
                const HeldRecords& held, const SlotBases& bases, bool annotate)
      : volume_(&volume),
        held_(&held),
        waiting_(&held.WaitingRecords()),
        annotate_(annotate),
        pool_(threads - 1) {
    contexts_.reserve(pool_.Slots());
    for (size_t slot = 0; slot < pool_.Slots(); slot++) {
      Context& context =
          *contexts_.emplace_back(std::make_unique<Context>(volume));
      context.record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
      context.record.SetExtensionSource(
          [&held](ULONGLONG record) { return held.FindExtension(record); });
      context.record.SetBaseSource(
          [&bases](ULONGLONG record) { return bases.BaseOf(record); });
      context.record.SetReadRefusal(&context.refused);
    }
  }

  // Starts parsing the count waiting records from first on, after the batch
  // before it was ended.
  void Begin(size_t first, size_t count) {
    End();
    first_ = first;
    count_ = count;
    if (outcomes_.size() < count) {
      outcomes_.resize(count);
    }
    const size_t chunks =
        (count + waiting_chunk_records - 1) / waiting_chunk_records;
    // The logs of the batch before stay as they are: the lines its last record
    // logged when its attributes were dropped are replayed with the next one.
    log_set_ = 1 - log_set_;
    std::vector<Log::Detail::CapturedLog>& logs = logs_.at(log_set_);
    if (logs.size() < chunks) {
      logs.resize(chunks);
    }
    // Asked here, on the thread that owns the sinks: a worker never does.
    levels_ = Log::Detail::EnabledLevels();
    waited_chunk_ = no_chunk;
    active_ = true;
    pool_.Start(chunks,
                [this](size_t chunk, size_t slot) { ParseChunk(chunk, slot); });
  }

  // Waits for the threads that still parse, and drops the chunks not started.
  void End() {
    if (active_) {
      active_ = false;
      pool_.Finish();
    }
  }

  // Whether the batch being parsed holds waiting record index.
  [[nodiscard]] bool Covers(size_t index) const noexcept {
    return active_ && index >= first_ && index < first_ + count_;
  }

  // The outcome of waiting record index, once its chunk is parsed. index MUST
  // be covered. The reference stays valid until the next Begin().
  SlotOutcome& Take(size_t index) {
    const size_t chunk = (index - first_) / waiting_chunk_records;
    if (chunk != waited_chunk_) {
      pool_.WaitFor(chunk);
      waited_chunk_ = chunk;
    }
    return outcomes_.at(index - first_);
  }

  // The log that holds the lines the parse of waiting record index logged,
  // which its outcome marks. It stays valid until the second Begin() after the
  // batch.
  [[nodiscard]] const Log::Detail::CapturedLog& LogOf(size_t index) const {
    return logs_.at(log_set_).at((index - first_) / waiting_chunk_records);
  }

 private:
  // The FileRecord of one thread, and whether a parse of it wanted a read.
  struct Context {
    explicit Context(const NtfsVolume<S>& volume) : record(volume) {}

    Io::FileRecord<S> record;
    bool refused{false};
  };

  // The chunk number of no chunk.
  static constexpr size_t no_chunk = (std::numeric_limits<size_t>::max)();

  // Parses the records of one chunk on thread slot.
  void ParseChunk(size_t chunk, size_t slot) {
    Log::Detail::CapturedLog& log = logs_.at(log_set_).at(chunk);
    log.Reset(levels_);
    const Log::Detail::CaptureScope capture(log);
    Context& context = *contexts_.at(slot);

    const size_t first = chunk * waiting_chunk_records;
    const size_t last = (std::min)(first + waiting_chunk_records, count_);
    for (size_t offset = first; offset < last; offset++) {
      SlotOutcome& outcome = outcomes_.at(offset);
      outcome = SlotOutcome{};
      const HeldRecords::Waiting& waiting = waiting_->at(first_ + offset);
      // Its reads are the scan's thread's, and so is its place in the log.
      if (!waiting.slot || !waiting.list_extents.empty() ||
          !waiting.unheld_extensions.empty()) {
        outcome.needs_scan_thread = true;
        continue;
      }
      context.refused = false;
      outcome.log_begin = log.End();
      ParseWaiting(context.record, waiting, outcome);
      outcome.log_end = log.End();
      // What the attributes log when they are dropped is kept apart: it
      // belongs after what the scan does with the record.
      context.record.ClearAttrs();
      outcome.release_end = log.End();
      if (context.refused) {
        outcome = SlotOutcome{};
        outcome.needs_scan_thread = true;
      }
    }
  }

  // Parses waiting, whose bytes are held, as the second pass does on the
  // scan's thread: its header, then its attributes with the extension records
  // the first pass held.
  void ParseWaiting(Io::FileRecord<S>& file_record,
                    const HeldRecords::Waiting& waiting,
                    SlotOutcome& outcome) const {
    outcome.kind = SlotKind::Base;
    const bool parsed = file_record.ParseFileRecord(
        waiting.record, held_->Bytes(*waiting.slot));
    outcome.kept =
        parsed &&
        ReadBaseEntry(file_record, *volume_, waiting.record,
                      waiting.header_damaged, outcome.counts, outcome.entry);
    if (outcome.kept && annotate_) {
      outcome.note = ComputeDataNote(file_record);
    }
  }

  const NtfsVolume<S>* volume_;
  const HeldRecords* held_;
  const std::vector<HeldRecords::Waiting>* waiting_;
  bool annotate_;
  // The batch being parsed: its first waiting record, and its length.
  size_t first_{0};
  size_t count_{0};
  Log::Detail::LevelMask levels_{0};
  bool active_{false};
  // The last chunk Take() waited for, so that a chunk is waited for once.
  size_t waited_chunk_{no_chunk};
  std::vector<std::unique_ptr<Context>> contexts_;
  std::vector<SlotOutcome> outcomes_;
  // The capture of each chunk, for the batch being parsed and for the one
  // before it.
  std::array<std::vector<Log::Detail::CapturedLog>, 2> logs_;
  size_t log_set_{0};
  // Last: its threads use the members above, so it ends first.
  Util::ChunkPool pool_;
};

// The threads the scan parses with. options.threads gives it, except for 0: a
// volume with enough slots takes the threads of the machine, a small one takes
// one. A volume with a raw attribute callback takes one whatever is asked.
template <Cache::Strategy S>
size_t ScanThreads(const NtfsVolume<S>& volume,
                   const Mft::MftTree::ScanOptions& options, ULONGLONG total) {
  if (volume.HasAttrRawCB()) {
    return 1;
  }
  unsigned threads = options.threads;
  if (threads == 0) {
    if (total < min_auto_threaded_slots_override.value_or(
                    default_min_auto_threaded_slots)) {
      return 1;
    }
    threads = (std::min)(std::thread::hardware_concurrency(), max_auto_threads);
  }
  return (std::clamp)(threads, 1U, max_scan_threads);
}

// The two passes over the record slots of $MFT that make the entries of a
// scan, in record order, through one reused FileRecord. The first pass reads
// each slot, and keeps the entry of each base record. A base record with an
// $ATTRIBUTE_LIST waits for the second pass: by then the first one has held the
// extension records it asks for, so they come from memory instead of one disk
// read each. What does not fit in memory is read in the second pass, announced
// to the reader ahead of it.
//
// With more than one thread, the first pass has several threads parse the slots
// of each window, and takes their outcomes in record order. Only the thread
// that owns the scan reads the volume, logs, and changes the scan: a slot that
// needs a read is parsed again by it, as it is with one thread.
template <Cache::Strategy S>
class SlotScan {
 public:
  // entries and stats belong to the caller, and receive the result. bitmap,
  // when it holds a value, lists the slots in use: the scan leaves the others
  // alone.
  SlotScan(const NtfsVolume<S>& volume,
           const Mft::MftTree::ScanOptions& options,
           const std::optional<std::vector<BYTE>>& bitmap, bool annotate,
           std::vector<Mft::MftTree::Entry>& entries,
           Mft::MftTree::ScanStats& stats)
      : volume_(&volume),
        options_(&options),
        bitmap_(&bitmap),
        annotate_(annotate),
        entries_(&entries),
        stats_(&stats),
        total_(volume.GetRecordsCount()),
        held_(volume.GetFileRecordSize()),
        bases_(total_),
        unheld_(volume),
        file_record_(volume),
        resolver_(volume, entries, stats),
        slots_(volume) {
    file_record_.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
    file_record_.SetExtensionSource([this](ULONGLONG record) {
      const std::span<const BYTE> bytes = held_.FindExtension(record);
      return bytes.empty() ? unheld_.Find(record) : bytes;
    });
    file_record_.SetBaseSource(
        [this](ULONGLONG record) { return bases_.BaseOf(record); });
    if (bitmap) {
      slots_.Plan(PlanSlotRanges(*bitmap, total_));
    }
    parse_threads_ = ScanThreads(volume, options, total_);
    // An explicit count is used from the first window. A count left to the
    // scan waits for the probe.
    automatic_ = options.threads == 0;
  }

  SlotScan(const SlotScan&) = delete;
  SlotScan& operator=(const SlotScan&) = delete;
  SlotScan(SlotScan&&) = delete;
  SlotScan& operator=(SlotScan&&) = delete;

  // The attributes of the last slot a thread parsed are dropped here, as the
  // scan of one thread drops its own.
  ~SlotScan() {
    try {
      DropLastParsedByThread();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (...) {
      // A destructor cannot report a failure, and nothing is left to undo.
    }
  }

  // Reads the slots in record order, and keeps the entries.
  void FirstPass() {
    for (ULONGLONG record = 0; record < total_; record++) {
      if (!ProgressAllowsRecord(*options_, record, total_)) {
        stats_->complete = false;
        break;
      }

      held_.ReleaseBefore(record);
      if (*bitmap_ && IsMarkedFree(**bitmap_, record)) {
        stats_->skipped++;
        continue;
      }
      if (!TakeParsed(record)) {
        ParseHere(record);
      }
    }

    // The threads rest during the second pass, which they have no part in.
    if (parser_) {
      parser_->End();
    }
    held_.Finish();
  }

  // Parses the base records that wait for their extension records, and drops
  // the entries the scan does not keep.
  void SecondPass() {
    std::vector<size_t> dropped;
    const std::vector<HeldRecords::Waiting>& waiting_records =
        held_.WaitingRecords();
    ReadAnnouncer<S> announcer(*volume_, waiting_records);
    const bool on_threads = WaitingParsedByThreads(waiting_records.size());
    for (size_t index = 0; index < waiting_records.size(); index++) {
      announcer.Before(index);
      ReplayPendingRelease();
      const std::optional<bool> taken =
          on_threads ? TakeWaiting(index) : std::nullopt;
      const bool kept =
          taken ? *taken : ParseWaitingHere(waiting_records.at(index));
      if (!kept) {
        dropped.push_back(waiting_records.at(index).entry_index);
        continue;
      }
      if ((index + 1) % announce_restart_interval == 0) {
        announcer.Restart(index + 1);
      }
    }
    // The threads rest while the entries are resolved and dropped.
    if (waiting_parser_) {
      waiting_parser_->End();
    }
    if (annotate_) {
      resolver_.Flush();
    }
    DropEntries(*entries_, dropped);
  }

 private:
  // Whether the threads parse the count waiting records of the second pass. An
  // explicit thread count always has them. A count left to the scan has them
  // from min_auto_threaded_waiting records on, whatever the first pass found:
  // the second pass reads next to nothing, so it is bound by the CPU even
  // where the first one was bound by the volume.
  [[nodiscard]] bool WaitingParsedByThreads(size_t count) const {
    if (parse_threads_ <= 1) {
      return false;
    }
    return !automatic_ || count >= min_auto_threaded_waiting_override.value_or(
                                       default_min_auto_threaded_waiting);
  }

  // Parses waiting on this thread, with the reads that takes, and keeps what it
  // gives. True when its entry is kept.
  bool ParseWaitingHere(const HeldRecords::Waiting& waiting) {
    Mft::MftTree::Entry& entry = entries_->at(waiting.entry_index);
    // The bytes the first pass parsed: the header parses as it did then. A
    // record that was not held is read again, announced to the reader.
    const bool parsed =
        waiting.slot ? file_record_.ParseFileRecord(waiting.record,
                                                    held_.Bytes(*waiting.slot))
                     : file_record_.ParseFileRecord(waiting.record);
    parsed_here_ = true;
    // Read even when the record is dropped: the reader was told to.
    unheld_.Load(waiting.unheld_extensions);
    RecordCounts counts;
    const bool kept =
        parsed && ReadBaseEntry(file_record_, *volume_, waiting.record,
                                waiting.header_damaged, counts, entry);
    AddCounts(*stats_, counts);
    unheld_.Clear();
    if (kept && annotate_) {
      resolver_.Add(waiting.entry_index, ComputeDataNote(file_record_));
    }
    return kept;
  }

  // Takes waiting record index from the batch the threads parse, giving them
  // the batch when the scan reaches it, and keeps what they found. Nothing when
  // the scan's thread has to parse the record: it reads the volume. Else
  // whether its entry is kept.
  std::optional<bool> TakeWaiting(size_t index) {
    const std::vector<HeldRecords::Waiting>& waiting_records =
        held_.WaitingRecords();
    if (!waiting_parser_) {
      waiting_parser_.emplace(*volume_, parse_threads_, held_, bases_,
                              annotate_);
    }
    if (!waiting_parser_->Covers(index)) {
      const size_t batch =
          (std::max)(size_t{1}, waiting_batch_records_override.value_or(
                                    default_waiting_batch_records));
      waiting_parser_->Begin(index,
                             (std::min)(batch, waiting_records.size() - index));
    }
    SlotOutcome& outcome = waiting_parser_->Take(index);
    if (outcome.needs_scan_thread) {
      return std::nullopt;
    }
    waiting_parsed_by_threads++;

    const HeldRecords::Waiting& waiting = waiting_records.at(index);
    // The record before is gone first, so that what its attributes log comes
    // before the lines of this one.
    if (parsed_here_) {
      file_record_.ClearAttrs();
      parsed_here_ = false;
    }
    const Log::Detail::CapturedLog& log = waiting_parser_->LogOf(index);
    log.Replay(outcome.log_begin, outcome.log_end);
    AddCounts(*stats_, outcome.counts);
    if (outcome.kept) {
      entries_->at(waiting.entry_index) = std::move(outcome.entry);
      if (annotate_) {
        resolver_.Add(waiting.entry_index, std::move(outcome.note));
      }
    }
    LeaveRelease(log, outcome, waiting.record, held_.Bytes(*waiting.slot));
    return outcome.kept;
  }

  // Takes slot record from the window the threads parse, giving them the window
  // when the scan reaches it. False when the thread that owns the scan has to
  // parse the slot: no window serves it, or it needs the volume.
  bool TakeParsed(ULONGLONG record) {
    if (parse_threads_ <= 1 || record < serial_until_) {
      return false;
    }
    if (!parser_ || !parser_->Covers(record)) {
      if (parser_) {
        // The next window overwrites the bytes the threads parse.
        parser_->End();
      }
      const auto began = std::chrono::steady_clock::now();
      const std::optional<Mft::SlotWindow> window = slots_.ServeWindow(record);
      const auto loaded = std::chrono::steady_clock::now();
      if (!window) {
        return false;
      }
      if (!ThreadsParse(record, began, loaded)) {
        // This thread parses the window it just loaded.
        serial_until_ = window->last;
        return false;
      }
      if (!parser_) {
        parser_.emplace(*volume_, parse_threads_,
                        *bitmap_ ? &**bitmap_ : nullptr, annotate_);
      }
      parser_->Begin(*window);
    }

    SlotOutcome& outcome = parser_->Take(record);
    if (outcome.needs_scan_thread) {
      return false;
    }
    slots_parsed_by_threads++;
    if (outcome.kind == SlotKind::Base && outcome.has_list) {
      // The window holds the slot in full, so the record waits.
      if (!held_.HoldWaiting(record, slots_.HeldSlot(record), entries_->size(),
                             outcome.header_damaged,
                             std::move(outcome.list_extents))) {
        return false;
      }
      Merge(record, outcome);
      entries_->emplace_back();
      LeaveRelease(record, outcome);
      return true;
    }

    Merge(record, outcome);
    if (outcome.kind == SlotKind::Extension) {
      // Its base record may come later, and then takes it from memory.
      held_.MeetExtension(record, outcome.base_reference,
                          slots_.HeldSlot(record));
    }
    if (outcome.kind == SlotKind::Base && outcome.kept) {
      entries_->push_back(std::move(outcome.entry));
      if (annotate_) {
        resolver_.Add(entries_->size() - 1, std::move(outcome.note));
      }
    }
    LeaveRelease(record, outcome);
    return true;
  }

  // Whether the threads parse the window just loaded for slot record, which
  // took from began to loaded to load. With an explicit thread count, they
  // always do. With a count left to the scan, they do once a probe says they
  // pay: the first probe_slots slots are parsed by this thread alone, and the
  // time the windows took to load, against the time the probe lasted, tells
  // whether the volume is read faster than this thread parses. The choice is
  // made once.
  bool ThreadsParse(ULONGLONG record,
                    std::chrono::steady_clock::time_point began,
                    std::chrono::steady_clock::time_point loaded) {
    if (!automatic_) {
      return true;
    }
    if (probe_.decided) {
      return probe_.threads_pay;
    }
    if (!probe_.started) {
      probe_.started = true;
      probe_.first_slot = record;
      probe_.began = began;
    }
    probe_.load_seconds +=
        std::chrono::duration<double>(loaded - began).count();
    if (record - probe_.first_slot <
        probe_slots_override.value_or(default_probe_slots)) {
      return false;
    }
    probe_.decided = true;
    probe_.threads_pay = probe_verdict_override.value_or(ScanThreadsPay(
        probe_.load_seconds,
        std::chrono::duration<double>(loaded - probe_.began).count()));
    return probe_.threads_pay;
  }

  // Makes the scan take what the threads found in slot record: its log lines,
  // its counters, and the base it names. The slot before it is gone first, so
  // that what its attributes log comes before the lines of this one.
  void Merge(ULONGLONG record, const SlotOutcome& outcome) {
    ReplayPendingRelease();
    if (parsed_here_) {
      file_record_.ClearAttrs();
      parsed_here_ = false;
    }
    parser_->LogOf(record).Replay(outcome.log_begin, outcome.log_end);
    AddCounts(*stats_, outcome.counts);
    if (outcome.kind != SlotKind::Unreadable) {
      bases_.Note(record, outcome.base_reference);
    }
  }

  // Keeps the lines the attributes of slot record logged when a thread dropped
  // them, for the next slot to be parsed: one thread drops the attributes of a
  // slot when it parses the next one.
  void LeaveRelease(ULONGLONG record, const SlotOutcome& outcome) {
    LeaveRelease(parser_->LogOf(record), outcome, record, {});
  }

  // Same for a record in log, whose bytes are held. A record of the second
  // pass gives them, since no window holds it: held is empty for a slot of the
  // first pass.
  void LeaveRelease(const Log::Detail::CapturedLog& log,
                    const SlotOutcome& outcome, ULONGLONG record,
                    std::span<const BYTE> held) {
    if (outcome.release_end.lines > outcome.log_end.lines) {
      pending_release_ = {.log = &log,
                          .begin = outcome.log_end,
                          .end = outcome.release_end,
                          .slot = record,
                          .held = held};
    }
  }

  // Ends the scan for the attributes LeaveRelease() kept. One thread drops
  // the attributes of its last slot when its FileRecord goes away, which
  // does it member by member in the reverse of the order ClearAttrs() uses. To
  // log them in that order, the slot is parsed again, without a sound, in
  // file_record_, whose destructor then does it.
  void DropLastParsedByThread() {
    if (pending_release_.log == nullptr || !pending_release_.slot) {
      ReplayPendingRelease();
      return;
    }
    const std::span<const BYTE> bytes =
        pending_release_.held.empty() ? slots_.HeldSlot(*pending_release_.slot)
                                      : pending_release_.held;
    pending_release_.log = nullptr;
    if (bytes.empty()) {
      return;
    }
    Log::Detail::CapturedLog silent;
    silent.Reset(0);
    const Log::Detail::CaptureScope capture(silent);
    if (file_record_.ParseFileRecord(*pending_release_.slot, bytes)) {
      static_cast<void>(TryParseAttrs(file_record_));
    }
  }

  // Sends the lines LeaveRelease() kept to the sinks.
  void ReplayPendingRelease() {
    if (pending_release_.log != nullptr) {
      pending_release_.log->Replay(pending_release_.begin,
                                   pending_release_.end);
      pending_release_.log = nullptr;
    }
  }

  // Parses slot record on this thread, and keeps what it gives.
  void ParseHere(ULONGLONG record) {
    ReplayPendingRelease();
    RecordCounts counts;
    ParseHereInto(record, counts);
    AddCounts(*stats_, counts);
  }

  // The body of ParseHere(): counts receives what the slot adds to the
  // statistics.
  void ParseHereInto(ULONGLONG record, RecordCounts& counts) {
    bool header_damaged = false;
    const bool parsed = slots_.ParseFileRecord(file_record_, record);
    const SlotKind kind =
        ClassifySlot(file_record_, *volume_, parsed, counts, header_damaged);
    if (kind == SlotKind::Unreadable) {
      return;
    }
    bases_.Note(record, file_record_.GetBaseRecordReference());
    if (kind == SlotKind::Extension) {
      // Its base record may come later, and then takes it from memory.
      held_.MeetExtension(record, file_record_.GetBaseRecordReference(),
                          slots_.HeldSlot(record));
      return;
    }
    if (kind == SlotKind::Dropped) {
      return;
    }
    // Its entry keeps its place, so the entries stay in record order.
    const bool has_list = file_record_.HasAttrHeader(Attr::Type::AttributeList);
    if (has_list &&
        held_.HoldWaiting(
            record, slots_.HeldSlot(record), entries_->size(), header_damaged,
            file_record_.AttributeListExtents(max_announced_list_bytes))) {
      entries_->emplace_back();
      return;
    }
    if (has_list) {
      held_.Confirm(record);
    }

    // Filled in place: an Entry is big, and most slots are kept.
    Mft::MftTree::Entry& entry = entries_->emplace_back();
    parsed_here_ = true;
    if (!ReadBaseEntry(file_record_, *volume_, record, header_damaged, counts,
                       entry)) {
      entries_->pop_back();
      return;
    }
    if (annotate_) {
      resolver_.Add(entries_->size() - 1, ComputeDataNote(file_record_));
    }
  }

  const NtfsVolume<S>* volume_;
  const Mft::MftTree::ScanOptions* options_;
  const std::optional<std::vector<BYTE>>* bitmap_;
  bool annotate_;
  std::vector<Mft::MftTree::Entry>* entries_;
  Mft::MftTree::ScanStats* stats_;
  ULONGLONG total_;
  HeldRecords held_;
  SlotBases bases_;
  UnheldExtensions<S> unheld_;
  Io::FileRecord<S> file_record_;
  DataStateResolver<S> resolver_;
  // Reads $MFT in big blocks, not record by record, and only the used ones.
  Mft::SlotReader<S> slots_;
  // Threads that parse, this one included, and whether the scan chooses when
  // they start. With one, this thread parses every slot.
  size_t parse_threads_{1};
  bool automatic_{false};

  // What the probe of an automatic scan has seen so far.
  struct Probe {
    bool started{false};
    bool decided{false};
    bool threads_pay{false};
    ULONGLONG first_slot{0};
    std::chrono::steady_clock::time_point began;
    double load_seconds{0};
  };

  Probe probe_;
  // The slots before this one are in a window this thread parses itself.
  ULONGLONG serial_until_{0};
  // Whether file_record_ holds the attributes of a slot this thread parsed.
  bool parsed_here_{false};

  // The lines the attributes of the last slot a thread parsed logged when they
  // were dropped, which no slot has taken its turn to replay yet.
  struct PendingRelease {
    const Log::Detail::CapturedLog* log{nullptr};
    Log::Detail::CapturedLog::Mark begin;
    Log::Detail::CapturedLog::Mark end;
    // The slot those attributes belong to, and its bytes when no window of
    // slots holds them: a waiting record of the second pass.
    std::optional<ULONGLONG> slot;
    std::span<const BYTE> held;
  };

  PendingRelease pending_release_;
  // Last: its threads read the window of slots_, so they end first.
  std::optional<WindowParser<S>> parser_;
  // Starts with the second pass. Its threads read held_ and bases_, and its
  // logs may be the ones pending_release_ points at.
  std::optional<WaitingParser<S>> waiting_parser_;
};

}  // namespace

size_t MaxPendingRanges() {
  return max_pending_ranges_override != 0 ? max_pending_ranges_override
                                          : default_max_pending_ranges;
}

void SetAutoThreadLimitsForTests(std::optional<ULONGLONG> min_slots,
                                 std::optional<ULONGLONG> probe_slots,
                                 std::optional<bool> verdict,
                                 std::optional<size_t> min_waiting) {
  min_auto_threaded_slots_override = min_slots;
  probe_slots_override = probe_slots;
  probe_verdict_override = verdict;
  min_auto_threaded_waiting_override = min_waiting;
}

bool ScanThreadsPay(double load_seconds, double elapsed_seconds) noexcept {
  return elapsed_seconds > 0 &&
         load_seconds <= max_load_share * elapsed_seconds;
}

size_t MaxHeldRecordBytes() {
  return max_held_record_bytes_override.value_or(default_max_held_record_bytes);
}

void SetMaxHeldRecordBytesForTests(std::optional<size_t> limit) {
  max_held_record_bytes_override = limit;
}

void SetWaitingBatchRecordsForTests(std::optional<size_t> records) {
  waiting_batch_records_override = records;
}

size_t WaitingParsedByThreadsForTests(bool reset) noexcept {
  const size_t count = waiting_parsed_by_threads;
  if (reset) {
    waiting_parsed_by_threads = 0;
  }
  return count;
}

size_t SlotsParsedByThreadsForTests(bool reset) noexcept {
  const size_t count = slots_parsed_by_threads;
  if (reset) {
    slots_parsed_by_threads = 0;
  }
  return count;
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
    // Set while PathOfDirectory() builds the path. A chain of parents that
    // meets it has looped.
    bool building{false};
  };

  // One directory whose path PathOfDirectory() builds.
  struct PendingDirectory {
    const Mft::MftTree::Entry* entry;
    const Mft::MftTree::Name* primary;
    DirectoryPath* path;
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

// Reads the record slots of volume's $MFT into entries, in record order, in the
// two passes of SlotScan.
template <Cache::Strategy S>
void Mft::MftTree::Impl::ReadSlots(
    const NtfsVolume<S>& volume, const Mft::MftTree::ScanOptions& options,
    const std::optional<std::vector<BYTE>>& bitmap, bool annotate) {
  SlotScan<S> scan(volume, options, bitmap, annotate, entries, stats);
  scan.FirstPass();
  scan.SecondPass();
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
  // An Entry is big: each time the vector grows, it moves them all. Without a
  // bitmap, the slots are all there is to go by.
  entries.reserve(gsl::narrow<size_t>(
      (std::min)(bitmap ? CountUsedSlots(*bitmap, total) : total,
                 max_reserved_entries)));

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

// The path of directory through primary, its primary name, remembered once
// built: the path of the first ancestor already known, or of the root or the
// record where the chain breaks, joined to the names below it. The directories
// of a chain that loops are walked whole, one by one: each sees the loop close
// at itself.
const Mft::MftTree::Impl::DirectoryPath& Mft::MftTree::Impl::PathOfDirectory(
    const Mft::MftTree::Entry& directory,
    const Mft::MftTree::Name& primary) const {
  const auto [found, added] = directory_paths_.try_emplace(directory.record);
  DirectoryPath& own = found->second;
  if (!added && !own.building) {
    return own;
  }
  if (!added) {
    // Left building by a call that threw.
    own.path = WalkPath(directory, primary, own.lost);
    own.building = false;
    return own;
  }

  own.building = true;
  std::vector<PendingDirectory> pending{{&directory, &primary, &own}};
  const DirectoryPath* known = nullptr;
  std::optional<ULONGLONG> lost;
  bool looped = false;
  const Mft::MftTree::Name* name = &primary;
  while (true) {
    const ULONGLONG parent = name->parent_record;
    if (!name->parent_valid) {
      lost = parent;
      break;
    }
    if (parent == root_record) {
      break;
    }
    // parent_valid comes from IsValidParent(), which finds parent in the scan.
    const Mft::MftTree::Entry* parent_entry = Find(parent);
    assert(parent_entry != nullptr);
    const Mft::MftTree::Name* parent_name = PrimaryName(*parent_entry);
    if (parent_name == nullptr) {
      lost = parent;
      break;
    }
    const auto [parent_found, parent_added] =
        directory_paths_.try_emplace(parent);
    if (!parent_added) {
      looped = parent_found->second.building;
      known = &parent_found->second;
      break;
    }
    parent_found->second.building = true;
    pending.push_back({parent_entry, parent_name, &parent_found->second});
    name = parent_name;
  }

  if (looped) {
    for (const PendingDirectory& item : pending) {
      item.path->path = WalkPath(*item.entry, *item.primary, item.path->lost);
      item.path->building = false;
    }
    return own;
  }

  std::wstring path = known != nullptr ? known->path : std::wstring{};
  if (known != nullptr) {
    lost = known->lost;
  }
  // The names join as WalkPath() joins them, from the top down.
  for (const PendingDirectory& item : pending | std::views::reverse) {
    if (!path.empty() || !lost) {
      path += L'\\';
    }
    path += item.primary->name;
    item.path->path = path;
    item.path->lost = lost;
    item.path->building = false;
  }
  return own;
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
