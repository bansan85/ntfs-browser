#include <ntfs-browser/win-types.h>

#include <algorithm>
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

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/file-name.h"
#include "attr/resident.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"

namespace NtfsBrowser::Mft {

namespace {

// The root directory's record number, fixed by NTFS.
constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// Records between two progress() calls: often enough for a live progress
// line, rare enough to cost nothing next to a record read.
constexpr ULONGLONG progress_interval = 4096;

// The Filename side of a $FILE_NAME attribute. FileRecord<S> always wraps a
// $FILE_NAME in the AttrFileName over the resident type that matches S.
template <Cache::Strategy S>
const Filename& AsFilename(const Attr::AttrBase<S>& attr) {
  if constexpr (S == Cache::Strategy::NoCache) {
    return static_cast<const Attr::AttrFileName<Attr::AttrResidentNoCache,
                                                Cache::Strategy::NoCache>&>(
        attr);
  } else {
    return static_cast<const Attr::AttrFileName<Attr::AttrResidentFullCache,
                                                Cache::Strategy::FullCache>&>(
        attr);
  }
}

// Copies what the record fr just parsed says about its file into entry.
// Returns false when an attribute failed to parse: entry then holds what
// parsed before the failure.
template <Cache::Strategy S>
bool ReadEntry(Io::FileRecord<S>& file_record, ULONGLONG record,
               Mft::MftTree::Entry& entry) {
  bool parsed = false;
  try {
    parsed = file_record.ParseAttrs();
  } catch (const std::exception& e) {
    Log::Exception(e);
  }

  entry.record = record;
  entry.sequence = file_record.GetSequenceNumber();
  entry.in_use = !file_record.IsDeleted();
  entry.directory = file_record.IsDirectory();

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

  const Attr::AttrBase<S>* data = file_record.FindStream({});
  entry.size =
      data != nullptr ? data->GetDataSize() : file_record.GetFileSize();
  entry.allocated_size = data != nullptr ? data->GetAllocatedSize() : 0;

  file_record.GetFileTime(&entry.write_time, &entry.create_time,
                          &entry.access_time, &entry.change_time);
  entry.read_only = file_record.IsReadOnly();
  entry.hidden = file_record.IsHidden();
  entry.system = file_record.IsSystem();
  entry.archive = file_record.IsArchive();
  entry.compressed = file_record.IsCompressed();
  entry.encrypted = file_record.IsEncrypted();
  entry.sparse = file_record.IsSparse();

  return parsed;
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

}  // namespace

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

// Reads every record slot of volume's $MFT through one reused FileRecord,
// keeps each base record's entry, then links the entries into a tree.
template <Cache::Strategy S>
void Mft::MftTree::Impl::Scan(const NtfsVolume<S>& volume,
                              const Mft::MftTree::ScanOptions& options) {
  const ULONGLONG total = volume.GetRecordsCount();
  stats.slots = total;

  Io::FileRecord<S> file_record(volume);
  file_record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);

  for (ULONGLONG record = 0; record < total; record++) {
    // Every progress_interval records, the caller follows along or stops.
    if (options.progress && record % progress_interval == 0 &&
        !options.progress(record, total)) {
      stats.complete = false;
      break;
    }

    if (!file_record.ParseFileRecord(record)) {
      stats.unreadable++;
      continue;
    }
    if (file_record.IsExtensionRecord()) {
      stats.extensions++;
      continue;
    }
    if (file_record.IsDeleted()) {
      stats.deleted++;
      if (!volume.GetOptions().include_deleted) {
        continue;
      }
    } else {
      stats.in_use++;
    }

    Mft::MftTree::Entry entry;
    if (!ReadEntry(file_record, record, entry)) {
      stats.damaged++;
      if (!volume.GetOptions().recover_errors) {
        continue;  // Strict: a damaged record is dropped, not kept partial.
      }
    }
    by_record.emplace(record, entries.size());
    entries.push_back(std::move(entry));
  }

  if (options.progress && stats.complete) {
    options.progress(total, total);
  }

  Log::Info("MFT scan: {} slots, {} in use, {} deleted, {} unreadable",
            stats.slots, stats.in_use, stats.deleted, stats.unreadable);
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

    const Mft::MftTree::Entry* parent_entry = Find(parent);
    current = parent_entry != nullptr ? PrimaryName(*parent_entry) : nullptr;
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

}  // namespace NtfsBrowser::Mft
