#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "alloc/data-state.h"
#include "attr/file-name.h"
#include "attr/resident.h"
#include "io/file-record-attrs.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/slot-reader.h"
#include "mft/tree-testing.h"

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
  for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
       file_record.GetAttr(Attr::Type::FileName)) {
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

// Classifies one record slot and, when it is an entry to keep, reads it.
// Counts the outcome in stats. Returns nullopt for a slot that is not kept:
// unreadable, an extension, a freed record not asked for, or a damaged one
// when strict.
template <Cache::Strategy S>
std::optional<Mft::MftTree::Entry>
    ReadSlot(Io::FileRecord<S>& file_record, Mft::SlotReader<S>& slots,
             const NtfsVolume<S>& volume, ULONGLONG record,
             Mft::MftTree::ScanStats& stats) {
  if (!slots.ParseFileRecord(file_record, record)) {
    stats.unreadable++;
    return std::nullopt;
  }
  if (file_record.IsExtensionRecord()) {
    stats.extensions++;
    return std::nullopt;
  }
  if (file_record.IsDeleted()) {
    stats.deleted++;
    if (!volume.GetOptions().include_deleted) {
      return std::nullopt;
    }
  } else {
    stats.in_use++;
  }

  Mft::MftTree::Entry entry;
  if (!ReadEntry(file_record, record, entry)) {
    stats.damaged++;
    // Strict: a damaged record is dropped, not kept partial.
    if (!volume.GetOptions().recover_errors) {
      return std::nullopt;
    }
  }
  return entry;
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

void SetMaxPendingRangesForTests(size_t limit) {
  max_pending_ranges_override = limit;
}

class MftTree::Impl {
 public:
  std::vector<Mft::MftTree::Entry> entries;
  std::unordered_map<ULONGLONG, size_t> by_record;
  std::unordered_map<ULONGLONG, std::vector<ULONGLONG>> children;
  // Parallel to entries_.
  std::vector<bool> reachable;
  Mft::MftTree::ScanStats stats;

  template <Cache::Strategy S>
  void Scan(const NtfsVolume<S>& volume,
            const Mft::MftTree::ScanOptions& options);
  void Link();
  [[nodiscard]] const Mft::MftTree::Entry* Find(ULONGLONG record) const;
  [[nodiscard]] std::span<const ULONGLONG> Children(ULONGLONG dir_record) const;
  [[nodiscard]] bool IsValidParent(const Mft::MftTree::Entry& child,
                                   const Mft::MftTree::Name& name) const;
  [[nodiscard]] std::wstring
      PathThrough(const Mft::MftTree::Entry& entry,
                  const Mft::MftTree::Name& name,
                  std::optional<ULONGLONG>* lost_ancestor) const;
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

// Reads the record slots of volume's $MFT through one reused FileRecord, keeps
// each base record's entry, then links the entries into a tree. With
// include_deleted off, the slots $MFT's $BITMAP marks free are not read.
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

  Io::FileRecord<S> file_record(volume);
  file_record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
  // Deleted entries only exist with include_deleted, and the annotation reads
  // the bitmap: without both, the scan reads what it always did.
  const bool annotate =
      volume.GetOptions().include_deleted && options.annotate_data_state;
  DataStateResolver<S> resolver(volume, entries, stats);
  // Reads $MFT in big blocks, not record by record.
  Mft::SlotReader<S> slots(volume);
  // End of the slots the current block read is allowed to cover, bitmap only.
  ULONGLONG range_end = 0;

  for (ULONGLONG record = 0; record < total; record++) {
    // Every progress_interval records, the caller follows along or stops.
    if (options.progress && record % progress_interval == 0 &&
        !options.progress(record, total)) {
      stats.complete = false;
      break;
    }

    if (bitmap && IsMarkedFree(*bitmap, record)) {
      stats.skipped++;
      continue;
    }
    if (bitmap && record >= range_end) {
      range_end = ReadRangeEnd(*bitmap, record, total);
      slots.Expect(record, range_end);
    }

    std::optional<Mft::MftTree::Entry> entry =
        ReadSlot(file_record, slots, volume, record, stats);
    if (!entry) {
      continue;
    }
    by_record.emplace(record, entries.size());
    entries.push_back(std::move(*entry));
    if (annotate) {
      resolver.Note(file_record, entries.size() - 1);
    }
  }
  if (annotate) {
    resolver.Flush();
  }

  if (options.progress && stats.complete) {
    options.progress(total, total);
  }

  Log::Info(
      "MFT scan: {} slots, {} in use, {} deleted, {} unreadable, {} skipped",
      stats.slots, stats.in_use, stats.deleted, stats.unreadable,
      stats.skipped);
  if (annotate) {
    Log::Info("MFT scan data: {} intact, {} reallocated, {} unknown, {} bad",
              stats.data_intact, stats.data_reallocated, stats.data_unknown,
              stats.data_bad);
  }
  Link();
}

// Validates every name's parent reference, files each record under the
// parents its valid names give, then marks what the root reaches.
void Mft::MftTree::Impl::Link() {
  for (Mft::MftTree::Entry& entry : entries) {
    // Parents this entry is already filed under: two hard links in one
    // directory still list the record once.
    std::vector<ULONGLONG> filed_under;
    for (Mft::MftTree::Name& name : entry.names) {
      name.parent_valid = IsValidParent(entry, name);
      // Only a valid, non-DOS name files a record, and the root is filed
      // under nothing.
      if (!name.parent_valid || name.dos_only || entry.record == root_record) {
        continue;
      }
      if (std::ranges::find(filed_under, name.parent_record) !=
          filed_under.end()) {
        continue;
      }
      filed_under.push_back(name.parent_record);
      children[name.parent_record].push_back(entry.record);
    }
  }

  reachable.assign(entries.size(), false);
  if (const auto root = by_record.find(root_record); root != by_record.end()) {
    // by_record_ values index entries_; reachable_ is as long as entries_.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    reachable[root->second] = true;
  }

  std::vector<ULONGLONG> pending{root_record};
  while (!pending.empty()) {
    const ULONGLONG dir = pending.back();
    pending.pop_back();
    for (const ULONGLONG child : Children(dir)) {
      const size_t index = by_record.at(child);
      // by_record_ values index entries_; reachable_ is as long as entries_.
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

// Whether name's parent reference, read from child's record, names a
// directory the scan found. See the rules in mft/tree.h.
bool Mft::MftTree::Impl::IsValidParent(const Mft::MftTree::Entry& child,
                                       const Mft::MftTree::Name& name) const {
  // Only the root directory is filed under itself.
  if (name.parent_record == child.record) {
    return child.record == root_record;
  }

  const Mft::MftTree::Entry* parent = Find(name.parent_record);
  if (parent == nullptr) {
    return name.parent_record == root_record;
  }
  if (!parent->directory) {
    return false;
  }

  // The parent is the same one the name was filed under: unchecked, still
  // the same generation, or the generation NTFS freed right after.
  return Mft::IsSameRecordGeneration(name.parent_sequence, parent->sequence,
                                     parent->in_use);
}

// Joins name and its ancestors' primary names up to the root, or up to the
// record where the chain of valid parent references breaks.
std::wstring Mft::MftTree::Impl::PathThrough(
    const Mft::MftTree::Entry& entry, const Mft::MftTree::Name& name,
    std::optional<ULONGLONG>* lost_ancestor) const {
  if (lost_ancestor != nullptr) {
    lost_ancestor->reset();
  }
  if (entry.record == root_record) {
    return L"\\";
  }

  std::vector<const std::wstring*> parts;
  // Guards against a forged chain of parents that loops back on itself.
  std::unordered_set<ULONGLONG> visited{entry.record};
  const Mft::MftTree::Name* current = &name;
  std::optional<ULONGLONG> lost;

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

  if (lost_ancestor != nullptr) {
    *lost_ancestor = lost;
  }
  return path;
}

const std::vector<Mft::MftTree::Entry>& Mft::MftTree::Entries() const noexcept {
  return impl_->entries;
}

// The lookup behind MftTree::Find().
const Mft::MftTree::Entry* Mft::MftTree::Impl::Find(ULONGLONG record) const {
  const auto iterator = by_record.find(record);
  // by_record_ maps to indices of entries_.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return iterator == by_record.end() ? nullptr : &entries[iterator->second];
}

// The lookup behind MftTree::Children().
std::span<const ULONGLONG>
    Mft::MftTree::Impl::Children(ULONGLONG dir_record) const {
  const auto iterator = children.find(dir_record);
  if (iterator == children.end()) {
    return {};
  }
  return iterator->second;
}

const Mft::MftTree::Entry* Mft::MftTree::Find(ULONGLONG record) const {
  return impl_->Find(record);
}

std::span<const ULONGLONG> Mft::MftTree::Children(ULONGLONG dir_record) const {
  return impl_->Children(dir_record);
}

bool Mft::MftTree::IsReachable(ULONGLONG record) const {
  const auto iterator = impl_->by_record.find(record);
  if (iterator == impl_->by_record.end()) {
    return false;
  }
  // by_record_ values index entries_; reachable_ is as long as entries_.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->reachable[iterator->second];
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

const Mft::MftTree::ScanStats& Mft::MftTree::Stats() const noexcept {
  return impl_->stats;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
