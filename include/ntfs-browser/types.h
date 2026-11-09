#pragma once

// Types of the Scan interface (see scan.h). They use no other type of
// the library, and nothing of <windows.h>: a program that includes scan.h
// needs nothing else.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

// A point in time, as NTFS stores it: 100 ns ticks since 1601-01-01 00:00 UTC.
// No zone is applied. 0 means the volume holds no time.
struct Date {
  std::uint64_t ticks{0};

  // Ticks in one second.
  static constexpr std::uint64_t ticks_per_second = 10'000'000;
  // Seconds from 1601-01-01 to 1970-01-01, the Unix epoch.
  static constexpr std::uint64_t unix_epoch_seconds = 11'644'473'600;

  // Whether the volume holds a time. False for 0.
  [[nodiscard]] constexpr bool IsSet() const noexcept { return ticks != 0; }

  // Seconds since 1970-01-01 UTC, rounded down. Negative before 1970.
  [[nodiscard]] constexpr std::int64_t ToUnixSeconds() const noexcept {
    return static_cast<std::int64_t>(ticks / ticks_per_second) -
           static_cast<std::int64_t>(unix_epoch_seconds);
  }

  // The date of a count of seconds since 1970-01-01 UTC. A date before 1601
  // gives 0.
  [[nodiscard]] static constexpr Date
      FromUnixSeconds(std::int64_t seconds) noexcept {
    const std::int64_t since_1601 =
        seconds + static_cast<std::int64_t>(unix_epoch_seconds);
    return Date{since_1601 < 0 ? 0
                               : static_cast<std::uint64_t>(since_1601) *
                                     ticks_per_second};
  }

  [[nodiscard]] constexpr auto operator<=>(const Date&) const = default;
  [[nodiscard]] constexpr bool operator==(const Date&) const = default;
};

// How a Scan walks the volume.
enum class EngineKind : std::uint8_t {
  // The default. A scan that starts in a folder uses DirectoryFullCache. A scan
  // of the root of a volume uses MftList.
  Auto,
  // Reads the $MFT in record order. Lists every record: files with no parent
  // directory, and one entry for each hard link. Reports a percent.
  MftList,
  // Walks the directory indexes, like the NtfsDir sample, with no block cache.
  // Uses little memory.
  DirectoryNoCache,
  // The same walk with the block cache of the library. Faster on a folder tree.
  DirectoryFullCache
};

// File attribute bits of Entry::attributes. The values are those of the
// Windows FILE_ATTRIBUTE_* constants, so a caller on Windows MAY test them
// with its own.
namespace Attribute {

// The content cannot be changed.
inline constexpr std::uint32_t read_only = 0x0001;
// Left out of a normal listing.
inline constexpr std::uint32_t hidden = 0x0002;
// Used by the operating system.
inline constexpr std::uint32_t system = 0x0004;
// The entry is a folder.
inline constexpr std::uint32_t directory = 0x0010;
// Marked for backup.
inline constexpr std::uint32_t archive = 0x0020;
// Reserved by Windows. Only the directory engines see it.
inline constexpr std::uint32_t device = 0x0040;
// No other attribute is set. Only the directory engines see it.
inline constexpr std::uint32_t normal = 0x0080;
// Kept in memory when possible, and deleted soon. Only the directory engines
// see it.
inline constexpr std::uint32_t temporary = 0x0100;
// The content has holes that take no space.
inline constexpr std::uint32_t sparse = 0x0200;
// A link, a mount point or another reparse tag. Only the directory engines see
// it.
inline constexpr std::uint32_t reparse_point = 0x0400;
// The content is stored compressed.
inline constexpr std::uint32_t compressed = 0x0800;
// The content lives on slow storage. Only the directory engines see it.
inline constexpr std::uint32_t offline = 0x1000;
// Left out of the content index. Only the directory engines see it.
inline constexpr std::uint32_t not_content_indexed = 0x2000;
// The content is encrypted (EFS).
inline constexpr std::uint32_t encrypted = 0x4000;

}  // namespace Attribute

// Whether the file record of an entry is still the file's own.
enum class Standing : std::uint8_t {
  // The record is in use and belongs to this entry.
  Live,
  // The record is freed: the file is deleted. Only a scan with
  // Options::include_deleted lists it.
  Deleted,
  // The record is in use under another sequence number: another file took it
  // after this entry was written. The entry is a stale name of a deleted file.
  // Counts as deleted.
  Reused,
  // The record does not parse. Only a scan with Options::recover_errors lists
  // such an entry, with the data of its directory entry or none.
  Unreadable
};

// What is known about the data of a deleted file: whether the clusters of its
// unnamed data stream are probably still its own. A hint, not a proof.
enum class DataState : std::uint8_t {
  // Not worked out: a live file, a file with no unnamed data stream, or a scan
  // that was not asked (Options::deleted_data_state).
  NotChecked,
  // The clusters are free, no other file claims them, none is bad.
  Intact,
  // Another file claims a cluster: the content is probably lost.
  Reallocated,
  // A cluster is listed as bad.
  Bad,
  // $Bitmap could not tell.
  Unknown
};

// The condition of one cluster of a file. Same meaning and same values as the
// library's cluster accounting (a cluster has exactly one value).
enum class ClusterCondition : std::uint8_t {
  // The run has no usable cluster address. The clusters it declares are counted
  // here.
  Invalid,
  // Listed as bad by the volume.
  Bad,
  // Past the valid data of the stream: never written, reads as zeros.
  NeverWritten,
  // Taken by another live file, inside its valid data: the content is lost.
  ReallocatedWritten,
  // Taken by another live file, beyond its valid data: the old content is
  // probably still there.
  ReallocatedUnwritten,
  // Taken by another deleted file that was deleted later.
  OverwrittenThenFreed,
  // Taken by another deleted file, and nothing tells which one came last.
  Contested,
  // Marked in use by the volume, but no file the scan read claims it.
  AllocatedUnknownOwner,
  // The volume cannot say.
  Unknown,
  // Free, claimed by no other file, not bad: probably intact.
  Intact,
  // Intact, in a compressed stream whose unit holds a cluster in another state:
  // the unit no longer decompresses.
  LostWithUnit,
  // Live file: in use, and claimed by this file only.
  Consistent,
  // Live file: the volume says the cluster is free.
  FreeButClaimed,
  // Live file: another live file claims it too.
  CrossLinked,
  // A hole of a sparse stream. Not a cluster on the disk.
  Sparse,
  // Probably intact, but the content reads all zeros.
  Zeroed,
  // Probably intact, but NTFS reused it for its own metadata.
  OverwrittenByMetadata,
  // Probably intact, and the content looks like data.
  IntactChecked
};

// The number of values of ClusterCondition.
inline constexpr std::size_t cluster_condition_count = 18;

// How many clusters of a file are in each condition, over all its data
// streams. Only the MftList engine works it out, and only for a deleted file.
struct ClusterSummary {
  // Indexed by the value of ClusterCondition.
  std::array<std::uint64_t, cluster_condition_count> clusters{};

  // The clusters in the given condition.
  [[nodiscard]] constexpr std::uint64_t
      Count(ClusterCondition condition) const noexcept {
    return clusters.at(static_cast<std::size_t>(condition));
  }
};

// The stable name of one file on one volume, as a Scan gives it and as a Reader
// takes it. It is plain values: copy it, keep it after the callback that gave
// it, store it. It holds no view and no pointer.
//
// A handle names a record of the $MFT at one moment of its life: the record
// number and the times the record was reused (the sequence number). It also
// names which of the names of the record the entry came from. A Reader checks
// the record against the handle and fails when the record is another file's
// now, or is gone. A handle only means something for the volume that gave it:
// the volume serial number is part of it, and a Reader of another volume
// refuses it.
struct Handle {
  // The value of name_index when the engine did not know which name of the
  // record the entry came from. The directory engines see one name at a time
  // and do not read the record.
  static constexpr std::uint16_t unknown_name = 0xFFFF;

  // $MFT record number of the file.
  std::uint64_t record{0};
  // The sequence number the record had when the entry was made. 0 accepts any.
  std::uint16_t sequence{0};
  // Position of the name among the $FILE_NAME attributes of the record that
  // have a name, in the order of the record, DOS aliases included. See
  // Reader::GetFileNames(). unknown_name when the engine did not know.
  std::uint16_t name_index{unknown_name};
  // The directory this name is filed under. With unknown_name it picks the name
  // of the record that is filed under that directory.
  std::uint64_t parent_record{0};
  // Whether the entry named a deleted file. A record that is in use again is
  // then another file: the Reader refuses the handle.
  bool deleted{false};
  // The serial number of the volume, from its boot sector.
  std::uint64_t volume{0};

  [[nodiscard]] constexpr bool operator==(const Handle&) const = default;
};

// One file or directory that a scan found. Every view in it points into a
// buffer that the Scan owns: it is valid only during the callback that
// received it. Copy what you keep.
struct Entry {
  // The file name, in raw UTF-16 units as NTFS stores it. It MAY hold an
  // unpaired surrogate. Never empty.
  std::u16string_view name;
  // The path from the root of the volume, with '\' before each name and a
  // leading '\' ("\Docs\report.txt"). Empty unless Options::full_path is set.
  // For an entry that no directory reaches (MftList only), the path starts with
  // the placeholder "\<lost>" in place of the folders that are lost, and goes
  // on with the names that are known ("\<lost>\Docs\report.txt").
  std::u16string_view path;

  // $MFT record number of the file, and the times that record was reused.
  std::uint64_t record{0};
  std::uint16_t sequence{0};
  // The directory this name is filed under.
  std::uint64_t parent_record{0};
  std::uint16_t parent_sequence{0};

  // Size in bytes and bytes allocated on disk. The directory engines read the
  // copy of the $FILE_NAME in the directory entry, which NTFS refreshes only
  // when the file is renamed: it can lag. MftList reads the $DATA stream.
  std::uint64_t size{0};
  std::uint64_t allocated_size{0};

  Date created;
  Date modified;
  // The last change of the $MFT record, not of the content.
  Date mft_changed;
  Date accessed;

  // Bits of Attribute::.
  std::uint32_t attributes{0};
  bool is_directory{false};
  Standing standing{Standing::Live};

  // Names the record has, hard links included, DOS aliases left out. 0 when the
  // engine does not know: the directory engines see one name at a time. The
  // MftList engine gives one entry for each name, all with this count.
  std::uint16_t link_count{0};

  // Only for a deleted entry, and when the engine worked it out.
  DataState data_state{DataState::NotChecked};
  // Cluster counts of a deleted file. Null when the engine did not work them
  // out. The directory engines never do: they only give data_state.
  const ClusterSummary* clusters{nullptr};

  // Which $FILE_NAME of the record this entry is: see Handle::name_index. Only
  // the MftList engine knows. Handle::unknown_name for the others.
  std::uint16_t name_index{Handle::unknown_name};
  // The serial number of the volume the entry comes from.
  std::uint64_t volume{0};

  // Whether the entry names a deleted file: a freed record, or a record that
  // another file reused.
  [[nodiscard]] constexpr bool IsDeleted() const noexcept {
    return standing == Standing::Deleted || standing == Standing::Reused;
  }

  // The stable name of this entry, which a Reader takes. Plain values: it stays
  // good after the views of the entry are gone.
  [[nodiscard]] constexpr Handle GetHandle() const noexcept {
    return {.record = record,
            .sequence = sequence,
            .name_index = name_index,
            .parent_record = parent_record,
            .deleted = IsDeleted(),
            .volume = volume};
  }
};

// How a Pattern compares its text with a name.
enum class PatternKind : std::uint8_t {
  // No search: every entry is delivered. The default.
  None,
  // The whole name is equal to the text.
  Exact,
  // The text may hold the wildcards '*' and '?'. See Pattern.
  Wildcard,
  // The text is a regular expression (PCRE2, Perl syntax). The library MAY be
  // built without it: see RegexAvailable() in scan.h. A scan that asks for it
  // then ends as Failed, and the log says "regex support is not compiled in".
  Regex
};

// What a Pattern is compared with.
enum class MatchTarget : std::uint8_t {
  // The file name alone ("report.txt"). A record with hard links gives one
  // entry for each of its names, and each name is tested on its own.
  Name,
  // The full path of the entry ("\Docs\report.txt"), as Entry::path holds it.
  // The scan then builds the path whatever Options::full_path says, and every
  // entry it delivers has its path set.
  FullPath
};

// The default of Pattern::regex_match_limit. A name is at most 255 units long:
// a pattern that needs more than a million steps on it is a runaway.
inline constexpr std::uint32_t default_regex_match_limit = 1000000;

// The default of Pattern::regex_jit_stack_bytes: 256 KiB. The stack of the
// compiled code grows from 32 KiB up to that, and the matching of a name takes
// a few KiB.
inline constexpr std::size_t default_regex_jit_stack_bytes = 256 * 1024;

// A search by name, applied by the scan itself: an entry that does not match is
// never delivered, never counted, and never takes memory in the buffers of the
// scan. An unset pattern (kind None) delivers everything.
//
// Names are raw UTF-16 units, and so is the text. An empty text matches no
// entry, as no name is empty.
//
// Exact. The name, or the path, equals the text on every unit.
//
// Wildcard. '*' matches any run of characters, an empty one included. '?'
// matches exactly one character. Every other unit of the text matches itself.
// There are no character classes and no escape character: a name that holds a
// '*' or a '?' is found with the wildcard that stands for it. A character is a
// code point: a surrogate pair counts as one character for '?' and for '*', and
// an unpaired surrogate counts as one character too. The other units of the
// text are compared one unit at a time. The match covers the whole name, not a
// part of it: "doc" does not find "doc.txt", "doc*" does. The time it takes is
// linear in the product of the lengths of the text and of the name, whatever
// the pattern.
//
// Case. Unless case_sensitive is set, two units are equal when the $UpCase
// table of the volume maps them to the same unit. The locale of the process is
// never used. $UpCase maps the units of the Basic Multilingual Plane: a
// character past it, written as a surrogate pair, is compared as it is. With
// case_sensitive the units are compared as they are.
//
// Regex. The text is a PCRE2 regular expression, compiled once when the scan
// starts. A text that does not compile ends the scan as Failed: the log gives
// the PCRE2 message and the position of the error. Regex is optional at build
// time (RegexAvailable() in scan.h). It is a search: the expression matches
// when it matches any part of the name, like "contains". Write '^' and '$' to
// hold it to the start and to the end. There is no other flag. The name, or the
// path, is matched as UTF-16 text, and \w, \d and \b follow Unicode. Unless
// case_sensitive is set, the name is first folded through the $UpCase table of
// the volume, and the expression is matched with PCRE2_CASELESS. So the
// expression sees a name in capitals: a property like \p{Ll} finds nothing in
// it. An unpaired surrogate in a name matches nothing, not even '.', so no
// match spans it. A name on which the engine gives up, because it hit
// regex_match_limit or the JIT stack, counts as no match. The first such name
// of a scan gets a warning in the log, and the others none. A text that matches
// every name, like "x*", is allowed. Nothing in a pattern is tied to the
// locale.
//
// FullPath. The separator '\' is a unit like another: '*' and '?' match it, so
// "\Docs\*.txt" finds the files of every folder below Docs. The path of an
// entry that no folder reaches (MftList only) starts with "\<lost>".
//
// Directories. A directory is an entry like a file: it is delivered when its
// own name or path matches, and only then. A directory that does not match is
// still entered when the scan is recursive, so the files below it are found.
struct Pattern {
  PatternKind kind{PatternKind::None};
  // The text to look for. Ignored for kind None.
  std::u16string text;
  // Compare units as they are, instead of through $UpCase.
  bool case_sensitive{false};
  MatchTarget target{MatchTarget::Name};
  // Regex only. The most steps PCRE2 spends on one name before it gives up, and
  // the most memory in bytes of the stack of the compiled code. See Regex
  // above. 0 is not a valid value: it is replaced by the default.
  std::uint32_t regex_match_limit{default_regex_match_limit};
  std::size_t regex_jit_stack_bytes{default_regex_jit_stack_bytes};
};

// A test on a number or a date: the value must lie between min and max. Both
// bounds are inclusive. A bound that is not set is not tested, so a range with
// no bound accepts every value, and a range with only min or only max is open
// on the other side.
template <typename T>
struct Range {
  std::optional<T> min;
  std::optional<T> max;

  // Whether at least one bound is set.
  [[nodiscard]] constexpr bool IsSet() const noexcept {
    return min.has_value() || max.has_value();
  }

  // Whether value lies between the bounds that are set, the bounds included.
  [[nodiscard]] constexpr bool Contains(const T& value) const noexcept {
    return (!min || !(value < *min)) && (!max || !(*max < value));
  }

  // Whether both bounds are set and min is above max: no value lies between
  // them.
  [[nodiscard]] constexpr bool IsEmpty() const noexcept {
    return min && max && *max < *min;
  }
};

// What Filter::deleted asks of the state of the entries.
enum class DeletedFilter : std::uint8_t {
  // Both live and deleted entries pass. The default.
  Any,
  // Only the entries with Standing::Live pass. An entry that is Unreadable is
  // neither live nor deleted: it does not pass.
  OnlyLive,
  // Only the entries that Entry::IsDeleted() accepts pass: Deleted and Reused.
  // The scan has to list them first: set Options::include_deleted.
  OnlyDeleted
};

// A test on the clusters of a deleted file: how many of them are in one
// condition. See Filter::clusters.
struct ClusterTest {
  ClusterCondition condition{ClusterCondition::Invalid};
  // The file has at least this many clusters in the condition. 0 sets no lower
  // bound.
  std::uint64_t at_least{0};
  // The file has at most this many clusters in the condition. Unset: no upper
  // bound. 0 asks for none at all.
  std::optional<std::uint64_t> at_most;
};

// The tests that an entry has to pass, on top of the Pattern. A scan applies
// them itself, like the Pattern: an entry that fails one is never delivered,
// never counted and never takes memory in the buffers of the scan.
//
// Every test is optional and every field starts unset. An unset field is not
// tested. The tests are combined with AND, and the Pattern is one more of them:
// an entry passes when it passes every test that is set. There is no OR and no
// NOT between tests. A caller that needs one runs two scans, or sets the
// inverse where a field has one (attributes_none, DeletedFilter::OnlyLive).
//
// Bounds. Every bound of a Range is inclusive. A range whose min is above its
// max can match nothing: the scan then ends as Failed and the log says which
// field. The same holds for a cluster test with at_most below at_least.
//
// Directories. A directory is an entry like a file and meets the same tests:
// a size range that starts at 1 drops every folder, as their size is 0 or the
// size of their index. A folder that fails a test is not delivered, and a
// recursive scan still enters it. Set is_directory to false for files only.
//
// Engines. Most fields are given by every engine. Some are not, and a filter on
// such a field is never left to match nothing or everything by accident:
//   - link_count and clusters: only the MftList engine knows them.
//   - the attribute bits device, normal, temporary, reparse_point, offline and
//     not_content_indexed, in attributes_all or attributes_none: only the
//     directory engines have them. The MftList engine reads the other bits of
//     the attributes of the record.
//   - the standings Reused and Unreadable in standing_in: only the directory
//     engines report them.
// A scan whose engine cannot give a field that the filter tests ends as
// Failed, and the log names the field. With EngineKind::Auto the scan picks the
// engine that can: MftList when the filter needs one of the fields of the first
// kind, a directory engine (the folder default) when it needs one of the second
// or third. A filter that needs both ends as Failed.
//
// Data. data_state_in and clusters look at the data of deleted files, which
// costs a read for each of them (see Options::deleted_data_state). A scan with
// such a test sets deleted_data_state itself, whatever the option says. Both
// only ever match a deleted entry that the scan lists: set
// Options::include_deleted, or nothing passes and the log warns of it. The same
// warning comes with DeletedFilter::OnlyDeleted.
//
// Not tested. The sequence numbers of the record and of the parent: they only
// tell one use of a record from another and have no meaning on their own. The
// name and the path have the Pattern.
struct Filter {
  // The size of the file, in bytes. See Entry::size.
  Range<std::uint64_t> size;
  // The bytes allocated on disk. See Entry::allocated_size.
  Range<std::uint64_t> allocated_size;

  // The four times of Entry. An entry whose time is not set on the volume
  // (ticks 0) fails a range on that time, whichever bounds it has.
  Range<Date> created;
  Range<Date> modified;
  Range<Date> mft_changed;
  Range<Date> accessed;

  // Bits of Attribute:: that the entry MUST all have, and bits that it MUST NOT
  // have. 0 tests nothing. A bit that is not in Attribute:: ends the scan as
  // Failed. An entry passes when (attributes & attributes_all) equals
  // attributes_all and (attributes & attributes_none) is 0. A bit in both masks
  // can never pass. See Filter for the bits that need a directory engine.
  std::uint32_t attributes_all{0};
  std::uint32_t attributes_none{0};

  // Only directories (true) or only files (false). Unset: both.
  std::optional<bool> is_directory;

  // Live or deleted entries. See DeletedFilter.
  DeletedFilter deleted{DeletedFilter::Any};
  // The standings that pass. Empty: not tested. Use it for what `deleted`
  // cannot say, like Reused or Unreadable.
  std::vector<Standing> standing_in;

  // The $MFT record number of the file. Unset bounds are not tested.
  Range<std::uint64_t> record;
  // The entry is filed under this directory record. Unset: not tested. The
  // root directory is record 5.
  std::optional<std::uint64_t> parent_record;

  // Names the record has. See Entry::link_count. Only the MftList engine
  // knows: with a directory engine the scan ends as Failed.
  Range<std::uint16_t> link_count;

  // The data states that pass. Empty: not tested. An entry with no state is
  // DataState::NotChecked, which is a value like another: list it to let live
  // files pass. Sets Options::deleted_data_state when it lists a state other
  // than NotChecked.
  std::vector<DataState> data_state_in;

  // Tests on the cluster counts of a deleted file. Every test has to pass. An
  // entry with no summary (see Entry::clusters), like every live file, fails
  // as soon as the list is not empty. Only the MftList engine works the
  // counts out: with a directory engine the scan ends as Failed. For example,
  // a file that is deleted and has no cluster that another file took over:
  // {.condition = ClusterCondition::ReallocatedWritten, .at_most = 0}.
  std::vector<ClusterTest> clusters;
};

// How far a scan has come.
struct Progress {
  // Entries delivered so far, all batches counted. With a Pattern it counts the
  // entries that matched: the others are dropped by the worker.
  std::uint64_t entries_found{0};
  // 0 to 100. Set by the MftList engine only. The directory engines cannot
  // know the size of the tree, so they leave it empty and the caller shows the
  // counter.
  std::optional<double> percent;
};

// What Scan::Poll() hands to the callback: the entries found since the last
// call, in the order they were found.
struct Batch {
  // The entries. A view into a buffer of the Scan, valid only during the
  // callback.
  std::span<const Entry> entries;
  Progress progress;
  // True for the last batch of the scan. No batch follows.
  bool last{false};
};

// Where a Scan stands.
enum class State : std::uint8_t {
  // Start() was not called.
  NotStarted,
  // The scan runs, or has entries that no Poll() has delivered.
  Running,
  // Every entry was found and delivered. The scan is over.
  Finished,
  // Cancel() stopped the scan. What was found was delivered.
  Cancelled,
  // The scan could not run: the target does not open, or is no NTFS volume, or
  // the pattern cannot be used. The log says why.
  Failed
};

// The default of Options::max_cache_bytes: 256 MiB.
inline constexpr std::uint64_t default_max_cache_bytes =
    std::uint64_t{256} * 1024 * 1024;

// The default of Options::max_pending_entries: 256 Ki entries. A batch of that
// size takes a few tens of MiB, and a poll every 100 ms never sees more.
inline constexpr std::size_t default_max_pending_entries = 262144;

// What to scan and how. Set a field before Scan() takes the options.
struct Options {
  // What to scan. A drive ("c:", "c:\Windows"), an image file or a device,
  // followed by an optional path inside the volume ("c:\image.img\Windows",
  // "/dev/sda1/Users"). Both '\' and '/' separate on Windows, only '/' on
  // Linux. With no inside path the scan covers the whole volume. A drive
  // letter is only known on Windows.
  std::u16string target;
  EngineKind engine{EngineKind::Auto};

  // Also scan every folder below the target folder. The MftList engine always
  // covers the whole volume: it filters the entries under the target folder.
  bool recursive{false};

  // The tolerance flags of the volume. Same meaning as the library's
  // VolumeOptions of the same names. include_deleted adds the deleted files,
  // recover_errors salvages damaged data and adds the entries of records that
  // do not parse.
  bool include_deleted{false};
  bool recover_errors{false};
  // Also list the system files of the volume ($MFT, $Bitmap, records 0 to 15).
  // They are hidden by default, as NtfsDir hides them.
  bool include_system_files{false};

  // Fill Entry::path. Costs one string for each entry. Always on when the
  // pattern looks at the full path.
  bool full_path{false};
  // Work out Entry::data_state of the deleted files. Costs a read of the file
  // record and of $Bitmap for each deleted entry. With the MftList engine it
  // also gives Entry::clusters: the scan then reads the cluster runs of every
  // record, which makes it slower and bigger. A Filter on the data state or on
  // the clusters switches it on, whatever this field says.
  bool deleted_data_state{true};

  // Memory the block cache of the DirectoryFullCache engine may hold, in
  // bytes. The other engines ignore it.
  std::uint64_t max_cache_bytes{default_max_cache_bytes};

  // Entries the scan may hold that Poll() has not delivered. When the scan
  // finds more than the caller takes, the worker thread waits for the next
  // Poll() instead of growing its buffer. The memory of a scan is therefore at
  // most twice this number of entries: the ones in fill and the ones the
  // callback reads. A batch is never larger. 0 sets no limit: the worker never
  // waits and the buffer grows with the volume. A caller that stops polling
  // blocks the worker at the limit. Cancel() and the destructor still end it.
  std::size_t max_pending_entries{default_max_pending_entries};

  // The search by name. Unset: every entry is delivered.
  Pattern pattern;

  // The tests on the other fields, joined with the pattern by AND. Unset:
  // every entry is delivered. See Filter.
  Filter filter;
};

}  // namespace NTFS_BROWSER_NAMESPACE
