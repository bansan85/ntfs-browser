#include "search/directory-engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "alloc/types.h"
#include "attr/mask.h"
#include "cache/strategy.h"
#include "index-entry.h"
#include "io/directory-read-ahead.h"
#include "io/file-record.h"
#include "io/record-standing.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "search/engine.h"
#include "search/match-sink.h"
#include "search/target.h"
#include "tuning/tuning.h"
#include "utf/utf.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// Separates the names of an Entry::path.
constexpr char16_t path_separator = u'\\';

// The bits of a $FILE_NAME flags word that are Win32 file attributes: the
// low 16. The directory and index view bits above them are dropped.
constexpr std::uint32_t attribute_bits = 0xFFFF;

// The standing of the record behind an index entry, as Entry reports it.
Standing StandingOf(Io::RecordStanding standing) {
  switch (standing) {
    case Io::RecordStanding::Live:
      return Standing::Live;
    case Io::RecordStanding::Deleted:
      return Standing::Deleted;
    case Io::RecordStanding::Reused:
      return Standing::Reused;
    case Io::RecordStanding::Unreadable:
      break;
  }
  return Standing::Unreadable;
}

// The data state of Alloc::DataState, as Entry reports it.
DataState DataStateOf(Alloc::DataState state) {
  switch (state) {
    case Alloc::DataState::Intact:
      return DataState::Intact;
    case Alloc::DataState::Reallocated:
      return DataState::Reallocated;
    case Alloc::DataState::Bad:
      return DataState::Bad;
    case Alloc::DataState::Unknown:
      break;
  }
  return DataState::Unknown;
}

// A directory that waits to be listed, as its name and its depth, not as its
// whole path: the paths of a tree that runs deep grow with its depth, and one
// for each directory that waits would cost memory that grows with the square
// of it.
struct Pending {
  ULONGLONG reference{0};
  std::u16string name;
  // Levels below the folder the scan starts at.
  std::size_t depth{0};
};

// The walk of one volume. Holds the state of the walk: the folder being
// listed, the folders that wait, and the ones listed.
template <Cache::Strategy S>
class DirectoryWalk {
 public:
  DirectoryWalk(const NtfsVolume<S>& volume, const Options& options,
                EntrySink& sink, std::stop_token stop)
      : volume_(volume),
        options_(options),
        sink_(sink),
        stop_(std::move(stop)),
        folder_(volume),
        probe_(volume) {
    folder_.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    probe_.SetAttrMask(Attr::Mask::Data);
  }

  // Enters the folder that inside names and lists it, and everything below it
  // when the options say so. Returns false when the folder cannot be entered.
  [[nodiscard]] bool Run(const std::vector<std::wstring>& inside) {
    if (!EnterTarget(inside)) {
      return false;
    }
    if (options_.recursive) {
      ListTree();
    } else {
      ListFolder();
    }
    return true;
  }

 private:
  // The entries and folders of one TraverseSubEntriesWithStanding pass.
  void OnEntry(const IndexEntryView& index_entry, Io::RecordStanding standing) {
    if (stop_.stop_requested() || !IsListed(index_entry)) {
      return;
    }
    const Standing entry_standing = StandingOf(standing);
    Entry entry = Describe(index_entry, entry_standing);
    if (entry_standing == Standing::Deleted && options_.deleted_data_state &&
        !entry.is_directory) {
      entry.data_state = DeletedDataState(entry.record);
    }
    if (options_.full_path) {
      path_.assign(folder_path_);
      path_ += path_separator;
      path_ += entry.name;
      entry.path = path_;
    }
    sink_.Add(entry);

    if (options_.recursive && entry.is_directory &&
        standing != Io::RecordStanding::Reused) {
      children_.push_back({.reference = entry.record,
                           .name = std::u16string(entry.name),
                           .depth = 0});
    }
  }

  // Whether the listing shows index_entry: not a system file (unless asked),
  // not a DOS alias, and with a name.
  [[nodiscard]] bool IsListed(const IndexEntryView& index_entry) const {
    if (!options_.include_system_files &&
        index_entry.GetFileReference() <
            static_cast<ULONGLONG>(Mft::Idx::User)) {
      return false;
    }
    return index_entry.IsWin32Name() && !index_entry.GetFilename().empty();
  }

  // Fills an Entry from a directory entry. Its views point at name_ and
  // nothing else: the sink copies them.
  Entry Describe(const IndexEntryView& index_entry, Standing standing) {
    name_ = Utf::WideToUtf16(index_entry.GetFilename());
    const FileTimesUtc times = index_entry.GetFileTimesUtc();
    Entry entry;
    entry.name = name_;
    entry.record = index_entry.GetFileReference();
    entry.sequence = index_entry.GetSequenceNumber();
    entry.parent_record = index_entry.GetParentReference();
    entry.parent_sequence = index_entry.GetParentSequenceNumber();
    entry.size = index_entry.GetFileSize();
    entry.allocated_size = index_entry.GetAllocatedSize();
    entry.created = {.ticks = times.create};
    entry.modified = {.ticks = times.write};
    entry.mft_changed = {.ticks = times.change};
    entry.accessed = {.ticks = times.access};
    entry.is_directory = index_entry.IsDirectory();
    entry.attributes =
        (static_cast<std::uint32_t>(index_entry.GetFilePermission()) &
         attribute_bits) |
        (entry.is_directory ? Attribute::directory : 0U);
    entry.standing = standing;
    entry.volume = volume_.GetSerialNumber();
    return entry;
  }

  // What the file record of a deleted file says about its unnamed data. Reads
  // the record with a FileRecord of its own, as the folder being listed keeps
  // its own. NotChecked when the record does not parse or holds no such
  // stream.
  DataState DeletedDataState(ULONGLONG record) {
    if (!probe_.ParseFileRecord(record) || !probe_.ParseAttrs()) {
      return DataState::NotChecked;
    }
    const std::optional<Alloc::DataState> state = probe_.GetDataState();
    return state ? DataStateOf(*state) : DataState::NotChecked;
  }

  // Lists the entries of the folder that folder_ holds.
  void ListFolder() {
    folder_.TraverseSubEntriesWithStanding(
        [this](const IndexEntryView& index_entry, Io::RecordStanding standing,
               void* /*context*/) { OnEntry(index_entry, standing); },
        nullptr);
  }

  // Moves folder_ to the root folder, then to each folder of inside in turn.
  // Builds folder_path_ from the names as the volume holds them.
  [[nodiscard]] bool EnterTarget(const std::vector<std::wstring>& inside) {
    if (!folder_.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)) ||
        !folder_.ParseAttrs()) {
      Log::Error("Cannot read root directory of the volume");
      return false;
    }
    for (const std::wstring& name : inside) {
      const std::optional<IndexEntry> found = folder_.FindSubEntry(name);
      if (!found) {
        Log::Error("Cannot find directory {}", Utf::WideToUtf8(name));
        return false;
      }
      if (!found->IsDirectory()) {
        Log::Error("{} is not a directory", Utf::WideToUtf8(name));
        return false;
      }
      if (!folder_.ParseFileRecord(found->GetFileReference()) ||
          !folder_.ParseAttrs()) {
        Log::Error("Cannot read directory {}", Utf::WideToUtf8(name));
        return false;
      }
      folder_path_ += path_separator;
      folder_path_ += Utf::WideToUtf16(found->GetFilename());
    }
    return true;
  }

  // Makes folder_ the next waiting folder that is new and parses. Returns
  // false when none is left. A folder that cannot be read, or that is listed
  // already, is logged and skipped: NTFS allows no hard link to a folder, so
  // an entry that leads back to one is a loop or a corruption.
  [[nodiscard]] bool EnterNextFolder(Io::DirectoryReadAhead<S>& ahead) {
    while (!pending_.empty() && !stop_.stop_requested()) {
      Pending next = std::move(pending_.back());
      pending_.pop_back();
      // Back to the parent's path: a waiting folder is never deeper than one
      // level below the last one listed, and its parent is on the way.
      lengths_.resize(next.depth);
      folder_path_.resize(lengths_.back());
      folder_path_ += path_separator;
      folder_path_ += next.name;
      if (listed_.contains(next.reference)) {
        Log::Warn("Skipping {}: directory already listed", Name(folder_path_));
        ahead.Release(next.reference);
        continue;
      }
      if (folder_.ParseFileRecord(next.reference) && folder_.ParseAttrs()) {
        listed_.insert(next.reference);
        lengths_.push_back(folder_path_.size());
        depth_ = next.depth;
        return true;
      }
      Log::Warn("Cannot read directory {}", Name(folder_path_));
      ahead.Release(next.reference);
    }
    return false;
  }

  // A path as UTF-8, for the log.
  static std::string Name(std::u16string_view path) {
    return Utf::WideToUtf8(Utf::Utf16ToWide(path));
  }

  // Lists folder_ and every folder below it, depth first, in index order.
  // The records and the index blocks of the next folders are read together
  // ahead of the listing unless the tuning of the volume turns that off.
  void ListTree() {
    lengths_ = {folder_path_.size()};
    if (const std::optional<ULONGLONG> start = folder_.GetFileReference()) {
      listed_.insert(*start);
    }
    Io::DirectoryReadAhead<S> ahead(volume_);
    ahead.Attach(folder_);
    ahead.Begin();
    std::vector<ULONGLONG> upcoming;
    do {
      children_.clear();
      ListFolder();
      // What was read ahead for this folder is of no use any more.
      if (const std::optional<ULONGLONG> current = folder_.GetFileReference()) {
        ahead.Release(*current);
      }
      // The stack pops the last pushed first: push in reverse to list the
      // subfolders in index order.
      for (auto child = children_.rbegin(); child != children_.rend();
           ++child) {
        pending_.push_back({.reference = child->reference,
                            .name = std::move(child->name),
                            .depth = depth_ + 1});
      }
      if (ahead.Enabled()) {
        PlanReadAhead(ahead, upcoming);
      }
    } while (EnterNextFolder(ahead));
  }

  // Tells the read ahead which folders come next: the top of the stack, and
  // not the ones already listed, which the walk skips.
  void PlanReadAhead(Io::DirectoryReadAhead<S>& ahead,
                     std::vector<ULONGLONG>& upcoming) const {
    upcoming.clear();
    for (auto next = pending_.rbegin();
         next != pending_.rend() && upcoming.size() < ahead.Window(); ++next) {
      if (!listed_.contains(next->reference)) {
        upcoming.push_back(next->reference);
      }
    }
    ahead.Plan(upcoming);
  }

  const NtfsVolume<S>& volume_;
  const Options& options_;
  EntrySink& sink_;
  std::stop_token stop_;
  // The folder being listed. Parsed again for each folder of the walk.
  Io::FileRecord<S> folder_;
  // Reads the record of a deleted file, apart from folder_.
  Io::FileRecord<S> probe_;

  // The path of the folder being listed. Its first lengths_[d] units are the
  // path of its ancestor d levels below the start, so the path of the next
  // folder is its parent's prefix and its own name.
  std::u16string folder_path_;
  std::vector<std::size_t> lengths_;
  std::size_t depth_{0};
  std::vector<Pending> pending_;
  // The subfolders that the pass over the current folder found.
  std::vector<Pending> children_;
  // Record numbers of the folders listed so far. An index entry gives a number
  // with its sequence apart, so a number alone identifies a folder.
  std::unordered_set<ULONGLONG> listed_;

  // Scratch for the entry being built: the sink copies them.
  std::u16string name_;
  std::u16string path_;
};

}  // namespace

template <Cache::Strategy S>
DirectoryEngine<S>::DirectoryEngine(Options options, TargetParts parts)
    : options_(std::move(options)), parts_(std::move(parts)) {}

template <Cache::Strategy S>
bool DirectoryEngine<S>::Run(EntrySink& sink, std::stop_token stop) {
  VolumeOptions volume_options;
  volume_options.include_deleted = options_.include_deleted;
  volume_options.recover_errors = options_.recover_errors;
  volume_options.max_cache_bytes = options_.max_cache_bytes;

  const OpenedVolume<S> opened = OpenVolume<S>(parts_, volume_options);
  if (!opened.volume) {
    return false;
  }
  const NtfsVolume<S>& volume = *opened.volume;
  volume.SetReadPattern(Tuning::Pattern::DependentSmall);
  const std::unique_ptr<MatchingSink> selected = MatchingSink::Make(
      sink, options_, volume.GetUpCaseTable(),
      S == Cache::Strategy::FullCache ? EngineKind::DirectoryFullCache
                                      : EngineKind::DirectoryNoCache);
  if (!selected) {
    return false;
  }
  DirectoryWalk<S> walk(volume, options_, *selected, std::move(stop));
  return walk.Run(parts_.inside);
}

template class DirectoryEngine<Cache::Strategy::FullCache>;
template class DirectoryEngine<Cache::Strategy::NoCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Search
