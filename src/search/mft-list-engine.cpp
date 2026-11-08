#include "search/mft-list-engine.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/volume-options.h>

#include "log/ntfs-common.h"
#include "search/engine.h"
#include "search/match-sink.h"
#include "search/target.h"
#include "utf/utf.h"
#include "util/local-file-time.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// The record of the root directory, where every path starts.
constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// Starts the path of an entry that no directory reaches, in place of the part
// of its path that is lost.
constexpr std::u16string_view lost_prefix = u"\\<lost>";

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

// The bits of Attribute:: that the entry of a record shows.
std::uint32_t AttributesOf(const Mft::MftTree::Entry& entry) {
  std::uint32_t bits = 0;
  const auto set = [&bits](bool on, std::uint32_t bit) {
    if (on) {
      bits |= bit;
    }
  };
  set(entry.read_only, Attribute::read_only);
  set(entry.hidden, Attribute::hidden);
  set(entry.system, Attribute::system);
  set(entry.directory, Attribute::directory);
  set(entry.archive, Attribute::archive);
  set(entry.sparse, Attribute::sparse);
  set(entry.compressed, Attribute::compressed);
  set(entry.encrypted, Attribute::encrypted);
  return bits;
}

// The names of a record that make an entry: every one but a DOS alias.
std::uint16_t CountLinks(const Mft::MftTree::Entry& entry) {
  std::uint16_t links = 0;
  for (const Mft::MftTree::Name& name : entry.names) {
    if (!name.dos_only) {
      ++links;
    }
  }
  return links;
}

// The cluster counts of an account, as the summary of an Entry.
ClusterSummary SummaryOf(const Alloc::ClusterTally& tally) {
  ClusterSummary summary;
  for (std::size_t i = 0; i < summary.clusters.size(); ++i) {
    summary.clusters.at(i) = tally.counts.at(i);
  }
  return summary;
}

// The Date of a FILETIME that the tree holds in local time.
Date DateOf(const FILETIME& local) {
  return {.ticks = Util::LocalToUtcTicks(local)};
}

// Moves the MFT scan's progress to the sink and tells it to stop when the scan
// is asked to.
class ProgressRelay {
 public:
  ProgressRelay(EntrySink& sink, std::stop_token stop)
      : sink_(sink), stop_(std::move(stop)) {}

  bool operator()(ULONGLONG done, ULONGLONG total) const {
    sink_.SetPercent(total == 0 ? 100.0
                                : 100.0 * static_cast<double>(done) /
                                      static_cast<double>(total));
    return !stop_.stop_requested();
  }

 private:
  EntrySink& sink_;
  std::stop_token stop_;
};

// Decides whether an entry sits in the folder the scan covers, for a scan that
// does not cover the whole volume.
class FolderFilter {
 public:
  FolderFilter(const Mft::MftTree& tree, ULONGLONG folder, bool recursive)
      : tree_(tree), folder_(folder), recursive_(recursive) {}

  // Whether name, a name of some record, is filed in the folder, or with
  // recursive below it, through valid parent references.
  [[nodiscard]] bool Contains(const Mft::MftTree::Name& name) {
    if (!name.parent_valid) {
      return false;
    }
    return recursive_ ? IsInFolder(name.parent_record)
                      : name.parent_record == folder_;
  }

 private:
  // Whether directory is the folder or has it as an ancestor. Remembers the
  // answer for every directory on the way, so a tree costs one step for each
  // directory.
  bool IsInFolder(ULONGLONG directory) {
    std::vector<ULONGLONG> trail;
    bool found = false;
    ULONGLONG current = directory;
    // A chain of parent references MAY loop on a damaged volume: it is cut
    // after as many steps as the tree has entries.
    for (std::size_t steps = 0; steps <= tree_.Entries().size(); ++steps) {
      if (current == folder_) {
        found = true;
        break;
      }
      if (const auto known = memo_.find(current); known != memo_.end()) {
        found = known->second;
        break;
      }
      trail.push_back(current);
      const std::optional<ULONGLONG> parent = ParentOf(current);
      if (!parent) {
        break;
      }
      current = *parent;
    }
    for (const ULONGLONG record : trail) {
      memo_[record] = found;
    }
    return found;
  }

  // The directory that the first valid name of record is filed under. Nothing
  // for the root, for a record the tree does not hold and for a record whose
  // names lead nowhere.
  [[nodiscard]] std::optional<ULONGLONG> ParentOf(ULONGLONG record) const {
    if (record == root_record) {
      return std::nullopt;
    }
    const Mft::MftTree::Entry* entry = tree_.Find(record);
    if (entry == nullptr) {
      return std::nullopt;
    }
    for (const Mft::MftTree::Name& name : entry->names) {
      if (!name.dos_only && name.parent_valid) {
        return name.parent_record;
      }
    }
    return std::nullopt;
  }

  const Mft::MftTree& tree_;
  ULONGLONG folder_;
  bool recursive_;
  std::unordered_map<ULONGLONG, bool> memo_;
};

// Moves through the folders of inside, from the root, and returns the record of
// the last one. Nothing, after a log line, when a name does not exist or is not
// a folder. The index lookup folds case through the $UpCase of the volume.
std::optional<ULONGLONG>
    FindFolder(const NtfsVolume<Cache::Strategy::NoCache>& volume,
               const std::vector<std::wstring>& inside) {
  Io::FileRecord<Cache::Strategy::NoCache> folder(volume);
  folder.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!folder.ParseFileRecord(root_record) || !folder.ParseAttrs()) {
    Log::Error("Cannot read root directory of the volume");
    return std::nullopt;
  }
  ULONGLONG record = root_record;
  for (const std::wstring& name : inside) {
    const std::optional<IndexEntry> found = folder.FindSubEntry(name);
    if (!found) {
      Log::Error("Cannot find directory {}", Utf::WideToUtf8(name));
      return std::nullopt;
    }
    if (!found->IsDirectory()) {
      Log::Error("{} is not a directory", Utf::WideToUtf8(name));
      return std::nullopt;
    }
    record = found->GetFileReference();
    if (!folder.ParseFileRecord(record) || !folder.ParseAttrs()) {
      Log::Error("Cannot read directory {}", Utf::WideToUtf8(name));
      return std::nullopt;
    }
  }
  return record;
}

// Gives the entries of a tree to a sink, one for each name of each record.
class TreeListing {
 public:
  TreeListing(const Mft::MftTree& tree, const Options& options, EntrySink& sink,
              std::stop_token stop)
      : tree_(tree), options_(options), sink_(sink), stop_(std::move(stop)) {}

  // Lists the records of the tree, in record order. With a filter, only the
  // names it accepts.
  void Run(FolderFilter* filter) {
    for (const Mft::MftTree::Entry& record : tree_.Entries()) {
      if (stop_.stop_requested()) {
        return;
      }
      if (!options_.include_system_files &&
          record.record < static_cast<ULONGLONG>(Mft::Idx::User)) {
        continue;
      }
      ListRecord(record, filter);
    }
  }

 private:
  // Gives one entry for each name of record that is no DOS alias.
  void ListRecord(const Mft::MftTree::Entry& record, FolderFilter* filter) {
    const std::uint16_t links = CountLinks(record);
    const bool deleted = !record.in_use;
    std::optional<ClusterSummary> summary;
    if (deleted && options_.deleted_data_state) {
      if (const Alloc::ClusterAccount* account =
              tree_.ClusterAccountOf(record.record)) {
        summary = SummaryOf(account->tally);
      }
    }
    for (std::size_t i = 0; i < record.names.size(); ++i) {
      const Mft::MftTree::Name& name = record.names.at(i);
      if (name.dos_only || (filter != nullptr && !filter->Contains(name))) {
        continue;
      }
      Entry entry = Describe(record, name, links);
      if (deleted) {
        entry.standing = Standing::Deleted;
        if (options_.deleted_data_state && record.data_state) {
          entry.data_state = DataStateOf(*record.data_state);
        }
        entry.clusters = summary ? &*summary : nullptr;
      }
      if (options_.full_path) {
        path_ = PathOf(record, i);
        entry.path = path_;
      }
      sink_.Add(entry);
    }
  }

  // Fills an Entry from a record and one of its names. Its name view points at
  // name_ and nothing else: the sink copies it.
  Entry Describe(const Mft::MftTree::Entry& record,
                 const Mft::MftTree::Name& name, std::uint16_t links) {
    name_ = Utf::WideToUtf16(name.name);
    Entry entry;
    entry.name = name_;
    entry.record = record.record;
    entry.sequence = record.sequence;
    entry.parent_record = name.parent_record;
    entry.parent_sequence = name.parent_sequence;
    entry.size = record.size;
    entry.allocated_size = record.allocated_size;
    entry.created = DateOf(record.create_time);
    entry.modified = DateOf(record.write_time);
    entry.mft_changed = DateOf(record.change_time);
    entry.accessed = DateOf(record.access_time);
    entry.attributes = AttributesOf(record);
    entry.is_directory = record.directory;
    entry.link_count = links;
    return entry;
  }

  // The path of record through its name number index. A path that the root does
  // not start gets lost_prefix, in place of the directories that are lost.
  [[nodiscard]] std::u16string PathOf(const Mft::MftTree::Entry& record,
                                      std::size_t index) const {
    std::optional<ULONGLONG> lost;
    std::u16string path =
        Utf::WideToUtf16(tree_.GetPath(record.record, index, &lost));
    if (lost) {
      path.insert(0, std::u16string(lost_prefix) + u'\\');
    }
    return path;
  }

  const Mft::MftTree& tree_;
  const Options& options_;
  EntrySink& sink_;
  std::stop_token stop_;

  // Scratch for the entry being built: the sink copies them.
  std::u16string name_;
  std::u16string path_;
};

}  // namespace

MftListEngine::MftListEngine(Options options, TargetParts parts)
    : options_(std::move(options)), parts_(std::move(parts)) {}

bool MftListEngine::Run(EntrySink& sink, std::stop_token stop) {
  VolumeOptions volume_options;
  volume_options.include_deleted = options_.include_deleted;
  volume_options.recover_errors = options_.recover_errors;

  const OpenedVolume<Cache::Strategy::NoCache> opened =
      OpenVolume<Cache::Strategy::NoCache>(parts_, volume_options);
  if (!opened.volume) {
    return false;
  }

  const std::unique_ptr<MatchingSink> selected = MatchingSink::Make(
      sink, options_, opened.volume->GetUpCaseTable(), EngineKind::MftList);
  if (!selected) {
    return false;
  }

  std::optional<ULONGLONG> folder;
  if (!parts_.inside.empty()) {
    folder = FindFolder(*opened.volume, parts_.inside);
    if (!folder) {
      return false;
    }
  }

  Mft::MftTree::ScanOptions scan_options;
  scan_options.progress = ProgressRelay(sink, stop);
  scan_options.annotate_data_state = options_.deleted_data_state;
  if (options_.include_deleted && options_.deleted_data_state) {
    scan_options.cluster_account = Alloc::ClusterAccountLevel::Counts;
  }
  const Mft::MftTree tree(*opened.volume, scan_options);
  if (stop.stop_requested()) {
    return true;
  }

  TreeListing listing(tree, options_, *selected, std::move(stop));
  if (folder) {
    FolderFilter filter(tree, *folder, options_.recursive);
    listing.Run(&filter);
  } else {
    listing.Run(nullptr);
  }
  sink.SetPercent(100.0);
  return true;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
