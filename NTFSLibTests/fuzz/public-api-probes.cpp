#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader-types.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/fuzz/looping-disk-reader.h>
#include <ntfs-browser/test/fuzz/public-api-probes.h>
#include <ntfs-browser/types.h>

#include "disk-reader.h"
#include "passthrough-key-provider.h"
#include "search/reader-probe.h"
#include "search/scan-probe.h"
#include "search/target.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz {

namespace {

using NTFS_BROWSER_NAMESPACE::AttributeInfo;
using NTFS_BROWSER_NAMESPACE::AttributeKey;
using NTFS_BROWSER_NAMESPACE::Batch;
using NTFS_BROWSER_NAMESPACE::ClusterCondition;
using NTFS_BROWSER_NAMESPACE::DataState;
using NTFS_BROWSER_NAMESPACE::DeletedFilter;
using NTFS_BROWSER_NAMESPACE::EngineKind;
using NTFS_BROWSER_NAMESPACE::Entry;
using NTFS_BROWSER_NAMESPACE::FileName;
using NTFS_BROWSER_NAMESPACE::Filter;
using NTFS_BROWSER_NAMESPACE::Handle;
using NTFS_BROWSER_NAMESPACE::MatchTarget;
using NTFS_BROWSER_NAMESPACE::Options;
using NTFS_BROWSER_NAMESPACE::PatternKind;
using NTFS_BROWSER_NAMESPACE::Reader;
using NTFS_BROWSER_NAMESPACE::ReaderOptions;
using NTFS_BROWSER_NAMESPACE::Scan;
using NTFS_BROWSER_NAMESPACE::StreamInfo;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

// The longest a scan MAY run before the watchdog cancels it. A hostile volume
// can declare far more work than a probe can wait for, and a clean one ends in
// a few milliseconds.
constexpr std::chrono::seconds scan_deadline{5};

// The pause between two polls of a scan. Short, so that a small volume does not
// cost the probe a tenth of a second.
constexpr std::chrono::milliseconds poll_interval{1};

// Entries a scan MAY give before the probe cancels it. Enough to walk a small
// volume whole, and a stop for a volume that declares millions.
constexpr std::uint64_t entry_cap = 2000;

// The pending entries a scan holds before its worker waits for a poll. A small
// number makes the worker meet that wait on any volume with a few files.
constexpr std::size_t pending_limit = 64;

// Handles of a scan that the Reader reads, after the records it reads by
// number.
constexpr std::size_t kept_handles = 16;

// Folder names and file names that a listing keeps for the later scans.
constexpr std::size_t kept_names = 3;

// How many bytes of a stream or an attribute one Read() call asks for. Small
// enough that a huge declared size cannot make a run long.
constexpr std::size_t read_bytes = 8192;

// The most bytes a copy takes from a stream before its sink refuses more: two
// chunks of the Reader.
constexpr std::uint64_t copy_cap = 2ULL * copy_chunk_bytes;

// A length that a ranged copy asks for: short, and not a chunk.
constexpr std::uint64_t copy_length = 100;

// A name index and an attribute type that no file has.
constexpr std::uint16_t absent_name_index = 1000;
constexpr AttributeType absent_attribute_type = 0xFFFFFFF0U;

// A record number past any record of a probed volume.
constexpr std::uint64_t absent_record = 0xFFFFFFFFULL;

// The largest value of a 64-bit counter: the open end of a range.
constexpr std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max();

// Folds value into seen. XOR cannot overflow, and a sanitizer of a fuzzing
// build then reports nothing for a field that holds any number.
void Mix(std::uint64_t& seen, std::uint64_t value) { seen ^= value; }

// A file in the temp folder, removed with the object. With no content it is a
// path that a target can name: SplitTarget() accepts a path that exists, and
// the reader factory then serves the input in place of the file. With content
// it is an image that the readers of the host open.
class TempFile {
 public:
  explicit TempFile(std::string_view stem, std::span<const BYTE> content = {}) {
    std::error_code error;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path(error);
    if (error) {
      return;
    }
    std::random_device random;
    path_ = folder /
            std::format("ntfs-fuzz-{}-{:08x}{:08x}", stem, random(), random());
    std::ofstream file(path_, std::ios::binary);
    file.write(reinterpret_cast<const char*>(content.data()),
               static_cast<std::streamsize>(content.size()));
    if (!file) {
      file.close();
      std::filesystem::remove(path_, error);
      path_.clear();
    }
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  ~TempFile() {
    if (!path_.empty()) {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }
  }

  // Whether the file was written.
  [[nodiscard]] bool Exists() const { return !path_.empty(); }

  // The scan target of the file: its path, or a path that does not exist when
  // the file could not be made. A scan then ends as Failed, which is a path
  // worth probing as well.
  [[nodiscard]] std::u16string Target() const {
    return path_.empty() ? std::u16string(u"ntfs-fuzz-no-such-file")
                         : path_.u16string();
  }

 private:
  std::filesystem::path path_;
};

// The empty target file of the process, made on first use.
const TempFile& SharedTarget() {
  static const TempFile file("target");
  return file;
}

// Makes every scan and every Reader read the input in place of the target, with
// a failing read when failing_read is set, and the EFS keys come from the
// input. Puts the real readers back when it goes out of scope.
class InputSeams {
 public:
  InputSeams(std::span<const BYTE> data,
             std::optional<std::size_t> failing_read) {
    NTFS_BROWSER_NAMESPACE::SetReaderFactoryForTests(
        [data, failing_read](const NTFS_BROWSER_NAMESPACE::TargetParts&)
            -> std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> {
          return std::make_unique<LoopingDiskReader>(data, failing_read);
        });
    NTFS_BROWSER_NAMESPACE::SetReaderKeyProviderForTests(
        std::make_shared<PassthroughKeyProvider>());
  }

  InputSeams(const InputSeams&) = delete;
  InputSeams& operator=(const InputSeams&) = delete;
  InputSeams(InputSeams&&) = delete;
  InputSeams& operator=(InputSeams&&) = delete;

  ~InputSeams() {
    NTFS_BROWSER_NAMESPACE::SetReaderFactoryForTests({});
    NTFS_BROWSER_NAMESPACE::ClearReaderKeyProviderForTests();
  }
};

// Makes the scans that start while it lives fill timings when they end. Takes
// the probe away when it goes out of scope.
class TimingScope {
 public:
  explicit TimingScope(NTFS_BROWSER_NAMESPACE::ScanTimings& timings) {
    NTFS_BROWSER_NAMESPACE::SetScanProbeForTests(&timings);
  }

  TimingScope(const TimingScope&) = delete;
  TimingScope& operator=(const TimingScope&) = delete;
  TimingScope(TimingScope&&) = delete;
  TimingScope& operator=(TimingScope&&) = delete;

  ~TimingScope() { NTFS_BROWSER_NAMESPACE::SetScanProbeForTests(nullptr); }
};

// Cancels a scan that runs past scan_deadline, from a thread of its own:
// Cancel() is the one call of Scan that another thread MAY make.
class Watchdog {
 public:
  explicit Watchdog(Scan& scan)
      : thread_([&scan, this](const std::stop_token& stop) {
          std::unique_lock lock(mutex_);
          // The predicate never holds: the wait ends on the deadline or on
          // the stop request of the destructor.
          (void)idle_.wait_for(lock, stop, scan_deadline, [] { return false; });
          if (!stop.stop_requested()) {
            scan.Cancel();
          }
        }) {}

 private:
  std::mutex mutex_;
  std::condition_variable_any idle_;
  // Declared last: the thread uses the members above.
  std::jthread thread_;
};

// What a listing gave that a later probe uses.
struct Listing {
  NTFS_BROWSER_NAMESPACE::State state{
      NTFS_BROWSER_NAMESPACE::State::NotStarted};
  std::vector<Handle> handles;
  std::vector<std::u16string> folders;
  std::vector<std::u16string> files;
};

// Reads every field of entry. A bad view or a wild pointer shows up here, in
// the sanitizers of a fuzzing build.
std::uint64_t ReadEntry(const Entry& entry) {
  std::uint64_t seen = 0;
  for (const char16_t unit : entry.name) {
    Mix(seen, unit);
  }
  for (const char16_t unit : entry.path) {
    Mix(seen, unit);
  }
  Mix(seen, entry.record);
  Mix(seen, entry.sequence);
  Mix(seen, entry.parent_record);
  Mix(seen, entry.parent_sequence);
  Mix(seen, entry.size);
  Mix(seen, entry.allocated_size);
  Mix(seen, entry.created.ticks);
  Mix(seen, entry.modified.ticks);
  Mix(seen, entry.mft_changed.ticks);
  Mix(seen, entry.accessed.ticks);
  Mix(seen, entry.attributes);
  Mix(seen, entry.link_count);
  Mix(seen, entry.name_index);
  Mix(seen, entry.volume);
  Mix(seen, entry.created.IsSet()
                ? static_cast<std::uint64_t>(entry.created.ToUnixSeconds())
                : 0);
  Mix(seen, static_cast<std::uint64_t>(entry.standing));
  Mix(seen, static_cast<std::uint64_t>(entry.data_state));
  Mix(seen, entry.IsDeleted() ? 1 : 0);
  Mix(seen, entry.GetHandle().record);
  Mix(seen, NTFS_BROWSER_NAMESPACE::ToUtf8(entry.name).size());
  Mix(seen, NTFS_BROWSER_NAMESPACE::EntryFlags(entry).size());
  if (entry.clusters != nullptr) {
    Mix(seen, entry.clusters->Count(ClusterCondition::Intact));
    Mix(seen, entry.clusters->Count(ClusterCondition::Bad));
  }
  return seen;
}

// Takes the handles and names of a batch into listing. Cancels scan once it has
// given entry_cap entries, or reports a percent that is neither the start nor
// the end: only a volume with more records than a probe MAY read says so.
void TakeBatch(const Batch& batch, Scan& scan, Listing& listing,
               std::uint64_t& entries) {
  std::uint64_t seen = 0;
  for (const Entry& entry : batch.entries) {
    Mix(seen, ReadEntry(entry));
    ++entries;
    if (listing.handles.size() < kept_handles) {
      listing.handles.push_back(entry.GetHandle());
    }
    std::vector<std::u16string>& names =
        entry.is_directory ? listing.folders : listing.files;
    if (names.size() < kept_names && !entry.name.empty()) {
      names.emplace_back(entry.name);
    }
  }
  Mix(seen, batch.progress.entries_found);
  const bool midway = batch.progress.percent && *batch.progress.percent > 0.0 &&
                      *batch.progress.percent < 100.0;
  if (entries >= entry_cap || midway) {
    scan.Cancel();
  }
  (void)seen;
}

// Runs a scan with options to its end and collects what it gave. The calls
// before Start() and after the end are probed too: they MUST do nothing.
Listing RunScan(const Options& options) {
  Listing listing;
  try {
    Scan scan(options);
    (void)scan.GetState();
    (void)scan.Finished();
    (void)scan.GetProgress();
    (void)scan.Poll();
    std::uint64_t entries = 0;
    scan.OnBatch(
        [&](const Batch& batch) { TakeBatch(batch, scan, listing, entries); });
    {
      const Watchdog watchdog(scan);
      listing.state = scan.Run(poll_interval);
    }
    (void)scan.Finished();
    (void)scan.GetProgress();
    (void)scan.Poll();
    scan.Start();
    scan.Cancel();
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (...) {
  }
  return listing;
}

// Starts a scan, polls it by hand and cancels it after the first batch.
void RunCancelledScan(const Options& options) {
  try {
    Scan scan(options);
    scan.OnBatch([&scan](const Batch& /*batch*/) { scan.Cancel(); });
    scan.Start();
    const Watchdog watchdog(scan);
    while (!scan.Finished()) {
      (void)scan.Poll();
      std::this_thread::sleep_for(poll_interval);
    }
    (void)scan.GetState();
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (...) {
  }
}

// A name that the volume and the Reader MUST treat as unknown.
constexpr std::u16string_view absent_name = u"ntfs-fuzz-absent";

// Reads one attribute of a file through every call of the Reader that takes a
// key: with the exact instance, with any instance, and from the middle.
void ReadAttributeOf(Reader& reader, const Handle& handle,
                     const AttributeInfo& attribute) {
  std::vector<std::uint8_t> buffer(read_bytes);
  AttributeKey key{.type = attribute.type,
                   .name = attribute.name,
                   .instance = attribute.instance};
  (void)reader.ReadAttribute(handle, key, 0, buffer);
  (void)reader.ReadAttribute(handle, key, attribute.size / 2, buffer);
  (void)reader.ReadAttribute(handle, key, attribute.size, buffer);
  key.instance = NTFS_BROWSER_NAMESPACE::any_instance;
  (void)reader.ReadAttribute(handle, key, 0, buffer);
}

// Reads one data stream of a file through every call of the Reader that takes a
// stream name: Read() at the start, the middle and the end, the data state, and
// the copies that finish, stop at a short length, stop at the cap of the sink,
// and stop at the first chunk.
void ReadStream(Reader& reader, const Handle& handle,
                const StreamInfo& stream) {
  std::vector<std::uint8_t> buffer(read_bytes);
  (void)reader.Read(handle, stream.name, 0, buffer);
  (void)reader.Read(handle, stream.name, stream.size / 2, buffer);
  (void)reader.Read(handle, stream.name, stream.size, buffer);
  (void)reader.Read(handle, stream.name, 0, std::span<std::uint8_t>());
  (void)reader.GetDataState(handle, stream.name);

  std::uint64_t taken = 0;
  const NTFS_BROWSER_NAMESPACE::ByteSink take_all =
      [&taken](std::span<const std::uint8_t> chunk) {
        taken += chunk.size();
        return true;
      };
  const NTFS_BROWSER_NAMESPACE::ByteSink take_some =
      [&taken](std::span<const std::uint8_t> chunk) {
        taken += chunk.size();
        return taken < copy_cap;
      };
  const NTFS_BROWSER_NAMESPACE::ByteSink refuse =
      [](std::span<const std::uint8_t> /*chunk*/) { return false; };
  (void)reader.CopyTo(handle, stream.name, take_all, 0, copy_length);
  (void)reader.CopyTo(handle, stream.name, take_all, stream.size / 2,
                      copy_length);
  (void)reader.CopyTo(handle, stream.name, take_all, stream.size, copy_length);
  (void)reader.CopyTo(handle, stream.name, take_some);
  (void)reader.CopyTo(handle, stream.name, refuse);
}

// Reads a file through every call of the Reader.
void ReadFile(Reader& reader, const Handle& handle) {
  std::uint64_t seen = 0;
  Mix(seen, static_cast<std::uint64_t>(reader.Check(handle).error));
  const auto info = reader.GetStandardInformation(handle);
  Mix(seen, info.value.created.ticks);
  Mix(seen, info.value.usn);
  Mix(seen, info.value.security_id);
  const auto names = reader.GetFileNames(handle);
  for (const FileName& name : names.value) {
    Mix(seen, name.name.size());
    Mix(seen, name.parent_record);
    Mix(seen, name.size);
  }
  (void)reader.GetFileName(handle);
  Handle indexed = handle;
  indexed.name_index = 0;
  (void)reader.GetFileName(indexed);
  indexed.name_index = absent_name_index;
  (void)reader.GetFileName(indexed);

  const auto streams = reader.GetStreams(handle);
  for (const StreamInfo& stream : streams.value) {
    ReadStream(reader, handle, stream);
  }
  std::vector<std::uint8_t> buffer(read_bytes);
  (void)reader.Read(handle, absent_name, 0, buffer);
  (void)reader.CopyTo(handle, absent_name,
                      [](std::span<const std::uint8_t>) { return true; });
  (void)reader.GetDataState(handle);
  (void)reader.GetDataState(handle, absent_name);

  const auto attributes = reader.GetAttributes(handle);
  for (const AttributeInfo& attribute : attributes.value) {
    ReadAttributeOf(reader, handle, attribute);
  }
  (void)reader.ReadAttribute(handle, {.type = absent_attribute_type}, 0,
                             buffer);
  (void)seen;
}

// Handles that no live file has, made from handle: another volume, the other
// deleted flag, the next sequence number, and the sequence that takes any file.
std::vector<Handle> StaleHandles(const Handle& handle) {
  Handle other_volume = handle;
  other_volume.volume = handle.volume + 1;
  Handle flipped = handle;
  flipped.deleted = !handle.deleted;
  Handle resequenced = handle;
  resequenced.sequence = static_cast<std::uint16_t>(handle.sequence + 1);
  Handle any_sequence = handle;
  any_sequence.sequence = 0;
  return {other_volume, flipped, resequenced, any_sequence};
}

// Records a full Reader probe reads by number: the metadata files, then the
// first user files. The handle holds no sequence and no volume, so it takes any
// file.
constexpr std::uint64_t numbered_records = 24;

// Records a short Reader probe reads by number: $MFT, $MFTMirr, $LogFile and
// $Volume.
constexpr std::uint64_t short_numbered_records = 4;

// Reads the first count records by number, and some that do not exist. A saved
// input records this part alone, so the part MUST come first in every Reader
// probe, and its records MUST be the same ones in the same order.
void ReadNumbered(Reader& reader, std::uint64_t count) {
  for (std::uint64_t record = 0; record < count; ++record) {
    ReadFile(reader, Handle{.record = record});
  }
  ReadFile(reader, Handle{.record = absent_record});
  ReadFile(reader, Handle{.record = absent_record,
                          .sequence = 1,
                          .name_index = 0,
                          .deleted = true,
                          .volume = max_value});
}

// Opens a Reader on the input and reads the files by number, then the files of
// handles, which a scan gave, and the stale handles made from them.
void ProbeReader(const std::u16string& target, bool recover_errors,
                 std::uint64_t numbered, const std::vector<Handle>& handles,
                 std::size_t limit) {
  try {
    Reader reader(
        ReaderOptions{.target = target, .recover_errors = recover_errors});
    (void)reader.IsOpen();
    ReadNumbered(reader, numbered);
    std::size_t read = 0;
    for (const Handle& handle : handles) {
      if (read++ == limit) {
        break;
      }
      ReadFile(reader, handle);
      for (const Handle& stale : StaleHandles(handle)) {
        ReadFile(reader, stale);
      }
    }
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (...) {
  }
}

// A Reader on a target that does not exist: every call fails as NotOpen.
void ProbeClosedReader() {
  try {
    Reader reader(ReaderOptions{.target = u"ntfs-fuzz-no-such-file"});
    (void)reader.IsOpen();
    ReadFile(reader, Handle{.record = 5});
  }
  // NOLINTNEXTLINE(bugprone-empty-catch)
  catch (...) {
  }
}

// The options of a scan that reads the input under the flags of volume.
Options BaseOptions(const VolumeOptions& volume, EngineKind engine,
                    std::u16string target) {
  Options options;
  options.target = std::move(target);
  options.engine = engine;
  options.include_deleted = volume.include_deleted;
  options.recover_errors = volume.recover_errors;
  options.include_system_files = true;
  options.full_path = true;
  options.max_cache_bytes = volume.max_cache_bytes;
  options.max_pending_entries = pending_limit;
  return options;
}

// The scan of the whole volume in $MFT order, as the MftList engine gives it.
Options MftListOptions(const VolumeOptions& volume,
                       const std::u16string& target) {
  return BaseOptions(volume, EngineKind::MftList, target);
}

// The recursive walk of the whole volume through the directory index, with the
// cache.
Options WalkOptions(const VolumeOptions& volume, const std::u16string& target) {
  Options options = BaseOptions(volume, EngineKind::DirectoryFullCache, target);
  options.recursive = true;
  return options;
}

// target with the folders of inside after it, separated by slashes: a path
// works on every host.
std::u16string Inside(std::u16string target,
                      std::initializer_list<std::u16string_view> inside) {
  for (const std::u16string_view folder : inside) {
    target += u'/';
    target += folder;
  }
  return target;
}

// The recursive scan of the folder known_folder with engine.
Options Folder(const VolumeOptions& volume, EngineKind engine,
               const std::u16string& target) {
  Options options = BaseOptions(volume, engine, Inside(target, {known_folder}));
  options.recursive = true;
  return options;
}

// The date at or after which a range of the filters starts.
constexpr NTFS_BROWSER_NAMESPACE::Date early{.ticks = 1};
constexpr NTFS_BROWSER_NAMESPACE::Date late{.ticks = max_value};

// A filter that tests every field the MftList engine gives, and that most
// entries of a small volume pass or fail on one test only.
Filter FullFilter() {
  Filter filter;
  filter.size = {.min = 0, .max = 1U << 20U};
  filter.allocated_size = {.min = 0, .max = max_value};
  filter.created = {.min = early, .max = late};
  filter.modified = {.min = early, .max = late};
  filter.mft_changed = {.min = early, .max = late};
  filter.accessed = {.min = early, .max = late};
  filter.attributes_none = NTFS_BROWSER_NAMESPACE::Attribute::hidden;
  filter.attributes_all = 0;
  filter.is_directory = false;
  filter.deleted = DeletedFilter::Any;
  filter.standing_in = {NTFS_BROWSER_NAMESPACE::Standing::Live,
                        NTFS_BROWSER_NAMESPACE::Standing::Deleted};
  filter.record = {.min = 0, .max = max_value};
  filter.parent_record = 5;
  filter.link_count = {.min = 0, .max = 100};
  filter.data_state_in = {DataState::NotChecked, DataState::Intact,
                          DataState::Unknown, DataState::Bad,
                          DataState::Reallocated};
  filter.clusters = {{.condition = ClusterCondition::Intact,
                      .at_least = 0,
                      .at_most = max_value},
                     {.condition = ClusterCondition::Bad, .at_least = 0}};
  return filter;
}

// A filter that tests only what the directory engines give: attribute bits the
// MftList engine does not keep, and the standings it cannot tell.
Filter DirectoryFilter() {
  Filter filter;
  filter.attributes_all = NTFS_BROWSER_NAMESPACE::Attribute::normal |
                          NTFS_BROWSER_NAMESPACE::Attribute::archive;
  filter.attributes_none = NTFS_BROWSER_NAMESPACE::Attribute::offline;
  filter.standing_in = {NTFS_BROWSER_NAMESPACE::Standing::Live,
                        NTFS_BROWSER_NAMESPACE::Standing::Reused,
                        NTFS_BROWSER_NAMESPACE::Standing::Unreadable,
                        NTFS_BROWSER_NAMESPACE::Standing::Deleted};
  filter.deleted = DeletedFilter::OnlyDeleted;
  filter.data_state_in = {DataState::Intact, DataState::Reallocated};
  return filter;
}

// Filters that no entry can pass, or that no engine can serve, one for each
// reason the scan refuses to start: an inverted range for each field, a bit
// that does not exist, a condition that does not exist, a field of the wrong
// engine, and a request for deleted entries from a scan that has none.
std::vector<Filter> RejectedFilters() {
  using NTFS_BROWSER_NAMESPACE::Date;
  std::vector<Filter> filters;
  const auto add = [&filters](const auto& change) {
    Filter filter;
    change(filter);
    filters.push_back(filter);
  };
  add([](Filter& f) { f.size = {.min = 2, .max = 1}; });
  add([](Filter& f) { f.allocated_size = {.min = 2, .max = 1}; });
  add([](Filter& f) { f.created = {.min = Date{2}, .max = Date{1}}; });
  add([](Filter& f) { f.modified = {.min = Date{2}, .max = Date{1}}; });
  add([](Filter& f) { f.mft_changed = {.min = Date{2}, .max = Date{1}}; });
  add([](Filter& f) { f.accessed = {.min = Date{2}, .max = Date{1}}; });
  add([](Filter& f) { f.record = {.min = 2, .max = 1}; });
  add([](Filter& f) { f.link_count = {.min = 2, .max = 1}; });
  add([](Filter& f) {
    f.clusters = {
        {.condition = ClusterCondition::Intact, .at_least = 2, .at_most = 1}};
  });
  add([](Filter& f) { f.attributes_all = 0x80000000U; });
  add([](Filter& f) { f.attributes_none = 0x40000000U; });
  add([](Filter& f) {
    f.clusters = {{.condition = static_cast<ClusterCondition>(200)}};
  });
  add([](Filter& f) {
    f.link_count = {.min = 1};
    f.attributes_all = NTFS_BROWSER_NAMESPACE::Attribute::device;
  });
  add([](Filter& f) {
    f.standing_in = {NTFS_BROWSER_NAMESPACE::Standing::Reused};
  });
  add([](Filter& f) { f.deleted = DeletedFilter::OnlyDeleted; });
  add([](Filter& f) { f.deleted = DeletedFilter::OnlyLive; });
  return filters;
}

// Patterns of every kind, on the name and on the full path, with the case kept
// and folded: some match nothing, one is an invalid expression, and one makes
// the matcher run out of its limit.
std::vector<NTFS_BROWSER_NAMESPACE::Pattern>
    ProbedPatterns(const Listing& listing) {
  using NTFS_BROWSER_NAMESPACE::Pattern;
  std::vector<Pattern> patterns;
  const auto add = [&patterns](PatternKind kind, std::u16string text,
                               MatchTarget target, bool case_sensitive) {
    patterns.push_back({.kind = kind,
                        .text = std::move(text),
                        .case_sensitive = case_sensitive,
                        .target = target});
  };
  if (!listing.files.empty()) {
    add(PatternKind::Exact, listing.files.front(), MatchTarget::Name, false);
    add(PatternKind::Exact, listing.files.front(), MatchTarget::Name, true);
    add(PatternKind::Exact, listing.files.front(), MatchTarget::FullPath,
        false);
  }
  add(PatternKind::Exact, u"", MatchTarget::Name, false);
  add(PatternKind::Wildcard, u"*", MatchTarget::Name, false);
  add(PatternKind::Wildcard, u"?*.t*", MatchTarget::Name, true);
  add(PatternKind::Wildcard, u"\\*\\?", MatchTarget::FullPath, false);
  add(PatternKind::Wildcard, u"*a**b?\xD83D\xDC1C", MatchTarget::Name, false);
  add(PatternKind::Wildcard, u"", MatchTarget::Name, false);
  add(PatternKind::Regex, u".*", MatchTarget::Name, false);
  add(PatternKind::Regex, u"^[A-Za-z]+\\.(txt|dat)$", MatchTarget::Name, true);
  add(PatternKind::Regex, u"\\\\[^\\\\]*$", MatchTarget::FullPath, false);
  add(PatternKind::Regex, u"(", MatchTarget::Name, false);
  add(PatternKind::Regex, u"", MatchTarget::Name, false);
  Pattern limited{.kind = PatternKind::Regex,
                  .text = u"(a+)+$",
                  .regex_match_limit = 1,
                  .regex_jit_stack_bytes = 1};
  patterns.push_back(limited);
  patterns.push_back({.kind = static_cast<PatternKind>(200), .text = u"x"});
  return patterns;
}

// Runs the scans that need no name from the volume. Gives the listing of the
// whole volume, from which the other scans take their names.
Listing ListEngines(const VolumeOptions& volume, const std::u16string& target) {
  Listing listing;
  {
    NTFS_BROWSER_NAMESPACE::ScanTimings timings;
    const TimingScope scope(timings);
    Options options = MftListOptions(volume, target);
    listing = RunScan(options);
    options.engine = EngineKind::Auto;
    options.recursive = true;
    (void)RunScan(options);
  }
  Options walk = WalkOptions(volume, target);
  (void)RunScan(walk);
  walk.engine = EngineKind::DirectoryNoCache;
  walk.recursive = false;
  walk.full_path = false;
  walk.deleted_data_state = false;
  (void)RunScan(walk);
  walk.recursive = true;
  walk.full_path = true;
  walk.deleted_data_state = true;
  (void)RunScan(walk);
  return listing;
}

// Runs the scans of one folder or file of the volume, by name: every engine,
// with and without recursion, and the names that cannot be entered.
void ListInside(const VolumeOptions& volume, const std::u16string& target,
                const Listing& listing) {
  // The folder that the saved inputs of the folder probes name.
  (void)RunScan(Folder(volume, EngineKind::MftList, target));
  (void)RunScan(Folder(volume, EngineKind::DirectoryFullCache, target));
  std::vector<std::u16string> folders = listing.folders;
  if (folders.empty()) {
    folders.emplace_back(u"Docs");
  }
  // Every engine on the first folder, with and without recursion. The second
  // folder only has to be found again.
  for (const EngineKind engine :
       {EngineKind::Auto, EngineKind::MftList, EngineKind::DirectoryNoCache,
        EngineKind::DirectoryFullCache}) {
    for (const bool recursive : {false, true}) {
      Options options =
          BaseOptions(volume, engine, Inside(target, {folders.front()}));
      options.recursive = recursive;
      (void)RunScan(options);
    }
  }
  for (const EngineKind engine :
       {EngineKind::MftList, EngineKind::DirectoryFullCache}) {
    Options options =
        BaseOptions(volume, engine, Inside(target, {folders.back()}));
    options.recursive = true;
    (void)RunScan(options);
  }
  // A path that goes two levels down, with both kinds of separator.
  (void)RunScan(BaseOptions(volume, EngineKind::DirectoryFullCache,
                            Inside(target, {folders.front(), folders.back()}) +
                                u"\\" + folders.front()));
  // A file is not a folder, and a name that the volume lacks is not a folder.
  for (const EngineKind engine :
       {EngineKind::MftList, EngineKind::DirectoryFullCache}) {
    if (!listing.files.empty()) {
      (void)RunScan(
          BaseOptions(volume, engine, Inside(target, {listing.files.front()})));
    }
    (void)RunScan(BaseOptions(volume, engine, Inside(target, {absent_name})));
  }
}

// Runs the patterns on two engines, and the filters on the engine that serves
// them: the ones that fail are meant to.
void ListFiltered(const VolumeOptions& volume, const std::u16string& target,
                  const Listing& listing) {
  std::size_t turn = 0;
  for (const NTFS_BROWSER_NAMESPACE::Pattern& pattern :
       ProbedPatterns(listing)) {
    Options options = BaseOptions(
        volume,
        turn++ % 2 == 0 ? EngineKind::MftList : EngineKind::DirectoryNoCache,
        target);
    options.pattern = pattern;
    options.recursive = true;
    (void)RunScan(options);
  }

  Options options = BaseOptions(volume, EngineKind::Auto, target);
  options.recursive = true;
  options.filter = FullFilter();
  (void)RunScan(options);
  options.include_deleted = true;
  (void)RunScan(options);
  options.filter = DirectoryFilter();
  (void)RunScan(options);
  options.engine = EngineKind::DirectoryFullCache;
  (void)RunScan(options);
  // A filter that needs both kinds of engine, and one that the data state of
  // deleted files decides.
  options.engine = EngineKind::Auto;
  options.filter = FullFilter();
  options.filter.standing_in = {NTFS_BROWSER_NAMESPACE::Standing::Reused};
  (void)RunScan(options);
  for (const Filter& filter : RejectedFilters()) {
    Options rejected = BaseOptions(
        volume,
        turn++ % 2 == 0 ? EngineKind::MftList : EngineKind::DirectoryFullCache,
        target);
    rejected.filter = filter;
    (void)RunScan(rejected);
  }
}

// Runs the scans whose target or engine does not exist.
void ListNothing(const VolumeOptions& volume) {
  (void)RunScan(BaseOptions(volume, EngineKind::MftList, u""));
  (void)RunScan(
      BaseOptions(volume, EngineKind::Auto, u"ntfs-fuzz-no-such-file"));
  (void)RunScan(BaseOptions(volume, static_cast<EngineKind>(200),
                            SharedTarget().Target()));
#ifdef _WIN32
  // A drive letter names a volume in place of a file: the factory still serves
  // the input.
  (void)RunScan(BaseOptions(volume, EngineKind::MftList, u"X:"));
  (void)RunScan(BaseOptions(volume, EngineKind::DirectoryFullCache, u"X:\\"));
  (void)RunScan(BaseOptions(volume, EngineKind::Auto, u"X:\\Docs"));
#endif
}

}  // namespace

void ProbePublicApi(std::span<const BYTE> data, const VolumeOptions& options,
                    std::optional<std::size_t> failing_read, ApiDepth depth) {
  const std::u16string target = SharedTarget().Target();
  const InputSeams seams(data, failing_read);

  if (depth == ApiDepth::Short) {
    (void)RunScan(MftListOptions(options, target));
    (void)RunScan(WalkOptions(options, target));
    ProbeReader(target, options.recover_errors, short_numbered_records, {}, 0);
    return;
  }

  const Listing listing = ListEngines(options, target);
  RunCancelledScan(MftListOptions(options, target));
  RunCancelledScan(WalkOptions(options, target));
  ListInside(options, target, listing);
  ListFiltered(options, target, listing);
  ListNothing(options);
  ProbeReader(target, options.recover_errors, numbered_records, listing.handles,
              kept_handles);
  ProbeClosedReader();
}

void ProbeFileTarget(std::span<const BYTE> data, const VolumeOptions& options) {
  const TempFile file("image", data);
  if (!file.Exists()) {
    return;
  }
  const std::u16string target = file.Target();
  (void)RunScan(MftListOptions(options, target));
  (void)RunScan(WalkOptions(options, target));
  (void)RunScan(Folder(options, EngineKind::DirectoryNoCache, target));
  ProbeReader(target, options.recover_errors, short_numbered_records, {}, 0);
}

void ProbeApiOnce(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
                  const VolumeOptions& options, ApiProbe probe) {
  const std::u16string target = SharedTarget().Target();
  // The first volume that opens takes the reader. The probes open one only.
  auto holder =
      std::make_shared<std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>>(
          std::move(reader));
  NTFS_BROWSER_NAMESPACE::SetReaderFactoryForTests(
      [holder](const NTFS_BROWSER_NAMESPACE::TargetParts&) {
        return std::move(*holder);
      });
  NTFS_BROWSER_NAMESPACE::SetReaderKeyProviderForTests(
      std::make_shared<PassthroughKeyProvider>());
  switch (probe) {
    case ApiProbe::MftListScan:
      (void)RunScan(MftListOptions(options, target));
      break;
    case ApiProbe::DirectoryScan:
      (void)RunScan(WalkOptions(options, target));
      break;
    case ApiProbe::Reader:
      ProbeReader(target, options.recover_errors, numbered_records, {}, 0);
      break;
    case ApiProbe::MftListFolder:
      (void)RunScan(Folder(options, EngineKind::MftList, target));
      break;
    case ApiProbe::DirectoryFolder:
      (void)RunScan(Folder(options, EngineKind::DirectoryFullCache, target));
      break;
  }
  NTFS_BROWSER_NAMESPACE::SetReaderFactoryForTests({});
  NTFS_BROWSER_NAMESPACE::ClearReaderKeyProviderForTests();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz
