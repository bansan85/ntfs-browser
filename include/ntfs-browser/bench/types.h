#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

// The stretches of a run that are timed apart. A phase MAY nest in another
// (DataStateFlush and ClusterSweep sit inside the second pass and the scan),
// and MAY repeat: the report adds the repeats.
enum class Phase : std::uint8_t {
  // Opening the volume: boot sector, $Volume, $MFT.
  Open,
  // Pass 1 of MftTree: every record slot read once.
  Pass1,
  // Pass 2 of MftTree: the base records with an $ATTRIBUTE_LIST.
  Pass2,
  // One batch of data-state queries resolved against the cluster bitmap.
  DataStateFlush,
  // The cluster account: the sweep over the claims of the whole scan.
  ClusterSweep,
  // The content check inside the sweep.
  ContentCheck,
  // MftTree linking the entries into a tree.
  Link,
  // Loading and applying the change journal.
  JournalLoad,
  // ntfsdir walking its directories.
  Walk,
  // A tool printing its listing.
  Print,
  // A tool freeing its tree.
  Destroy
};

inline constexpr std::size_t phase_count = 11;

// What a read was for, as the caller that made it declared. Unknown reads are
// Other.
enum class ReadKind : std::uint8_t {
  Other,
  // The boot sector, or a record at a fixed address.
  Boot,
  // A window of $MFT slots read by the scan.
  MftWindow,
  // One file record read by reference (a directory, a record to parse).
  Record,
  // Extension records read in one run for a base record.
  ExtensionRecord,
  // An $ATTRIBUTE_LIST stream.
  AttributeList,
  // An $INDEX_ALLOCATION block.
  IndexBlock,
  // A window of the cluster bitmap.
  BitmapWindow,
  // File data.
  Data,
  // The change journal.
  Journal,
  // A run of $MFT slots probed for a directory walk.
  ProbeRun,
  // Clusters read by the content check.
  ContentCheck
};

inline constexpr std::size_t read_kind_count = 12;

// Why a read waited, as Win32DiskReader judges it when the read is served
// (see ReadClass in the documentation of the tooling):
//   Io      the read was announced and the caller waited for the disk: the
//           read was in flight, or queued behind the read-ahead window.
//   Window  the read was announced and already done, and other announced
//           ranges were held back by the read-ahead caps: the CPU is late, and
//           the window is what keeps the disk idle.
//   Ready   the read was announced and already done, and nothing was held
//           back: the disk was ahead of the CPU and had nothing left to read.
//   Plan    the read was not announced (or not matched): a dependency the
//           caller could not know earlier. The caller waited for the disk.
enum class ReadClass : std::uint8_t { Unclassified, Io, Window, Ready, Plan };

inline constexpr std::size_t read_class_count = 5;

// A thread that reports its CPU time when it ends.
enum class ThreadRole : std::uint8_t {
  // The thread that called Start(), reported by Stop().
  Main,
  // A thread of Util::ChunkPool.
  PoolWorker,
  // The writer thread of the tools' buffered listing.
  OutputWriter,
  // The worker of Io::ReadAheadDiskReader.
  ReadAhead,
  Other
};

inline constexpr std::size_t thread_role_count = 5;

// The parts of the main thread's time inside a scan that are timed apart. Each
// is exclusive: the time of a slice nested in another is taken out of it, and
// so is the time the thread spent waiting for the disk (Report::read_wait_ns).
enum class Slice : std::uint8_t {
  // Parsing records on the main thread.
  Parse,
  // Taking and merging the outcomes of other threads, in order.
  Merge,
  // In Util::ChunkPool::WaitFor(): waiting for the threads that parse, or
  // parsing a chunk nobody took.
  PoolWait,
  // Sending the captured log lines to the sinks.
  LogReplay
};

inline constexpr std::size_t slice_count = 4;

// Names, for the report and the command lines.
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view
    PhaseName(Phase phase) noexcept;
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view
    ReadKindName(ReadKind kind) noexcept;
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view
    ReadClassName(ReadClass cls) noexcept;
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view
    ThreadRoleName(ThreadRole role) noexcept;
[[nodiscard]] NTFS_BROWSER_EXPORT std::string_view
    SliceName(Slice slice) noexcept;

// The phase called name ("pass1"), or nothing.
[[nodiscard]] NTFS_BROWSER_EXPORT std::optional<Phase>
    ParsePhase(std::string_view name) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
