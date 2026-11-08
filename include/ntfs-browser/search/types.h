#pragma once

// Types of the Search::Scan interface (see scan.h). They use no other type of
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

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Search {

// A point in time, as NTFS stores it: 100 ns ticks since 1601-01-01 00:00 UTC.
// No zone is applied. 0 means the volume holds no time.
struct Date {
  std::uint64_t ticks{0};

  // Ticks in one second.
  static constexpr std::uint64_t ticks_per_second = 10'000'000;
  // Seconds from 1601-01-01 to 1970-01-01, the Unix epoch.
  static constexpr std::uint64_t unix_epoch_seconds = 11'644'473'600;

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

inline constexpr std::uint32_t read_only = 0x0001;
inline constexpr std::uint32_t hidden = 0x0002;
inline constexpr std::uint32_t system = 0x0004;
inline constexpr std::uint32_t directory = 0x0010;
inline constexpr std::uint32_t archive = 0x0020;
inline constexpr std::uint32_t device = 0x0040;
inline constexpr std::uint32_t normal = 0x0080;
inline constexpr std::uint32_t temporary = 0x0100;
inline constexpr std::uint32_t sparse = 0x0200;
inline constexpr std::uint32_t reparse_point = 0x0400;
inline constexpr std::uint32_t compressed = 0x0800;
inline constexpr std::uint32_t offline = 0x1000;
inline constexpr std::uint32_t not_content_indexed = 0x2000;
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
  Invalid,
  Bad,
  NeverWritten,
  ReallocatedWritten,
  ReallocatedUnwritten,
  OverwrittenThenFreed,
  Contested,
  AllocatedUnknownOwner,
  Unknown,
  Intact,
  LostWithUnit,
  Consistent,
  FreeButClaimed,
  CrossLinked,
  Sparse,
  Zeroed,
  OverwrittenByMetadata,
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

  // Whether the entry names a deleted file: a freed record, or a record that
  // another file reused.
  [[nodiscard]] constexpr bool IsDeleted() const noexcept {
    return standing == Standing::Deleted || standing == Standing::Reused;
  }
};

// How far a scan has come.
struct Progress {
  // Entries found so far, all batches counted.
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
  // The scan could not run: the target does not open, or is no NTFS volume.
  // The log says why.
  Failed
};

// The default of Options::max_cache_bytes: 256 MiB.
inline constexpr std::uint64_t default_max_cache_bytes =
    std::uint64_t{256} * 1024 * 1024;

// What to scan and how. Set a field before Scan() takes the options. Fields
// of search patterns and filters are added after the ones below.
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

  // Fill Entry::path. Costs one string for each entry.
  bool full_path{false};
  // Work out Entry::data_state of the deleted files. Costs a read of the file
  // record and of $Bitmap for each deleted entry. With the MftList engine it
  // also gives Entry::clusters: the scan then reads the cluster runs of every
  // record, which makes it slower and bigger.
  bool deleted_data_state{true};

  // Memory the block cache of the DirectoryFullCache engine may hold, in
  // bytes. The other engines ignore it.
  std::uint64_t max_cache_bytes{default_max_cache_bytes};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
