#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

// Opt-in instrumentation of the read path: what the disk was asked, when, and
// who waited for it. It exists to prove whether the disk or the CPU bounds a
// run, and to tune the constants of Tuning::Settings. Off, it costs a
// predictable branch and changes nothing: the same reads, in the same order,
// with the same hints, the same log lines and the same output.
//
// A session is process-wide, like the logging sinks. Start() MUST come before
// the volume is opened, so that the reads of the open are logged, and from the
// thread that later calls Stop(). The session is not thread safe: only the
// worker threads that ThreadScope names report from outside the main thread.
// The report is plain data: the tools turn it into JSON, the library knows
// none.
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

// A histogram of non-negative values, by power of two: bucket i holds the
// values whose highest set bit is i, and bucket 0 also holds 0.
struct Histogram {
  std::array<std::uint64_t, 40> buckets{};
  std::uint64_t count{0};
  std::uint64_t sum{0};
  std::uint64_t max{0};
};

// What the runs of one phase cost. Wall time is the sum of the stretches,
// cpu_ns is the CPU time of the thread that ran the phase.
struct PhaseStat {
  Phase phase{Phase::Open};
  std::uint32_t count{0};
  std::uint64_t wall_ns{0};
  std::uint64_t cpu_ns{0};
  // When the first stretch began and the last one ended, from the session
  // start.
  std::uint64_t first_begin_ns{0};
  std::uint64_t last_end_ns{0};
  // The reads the volume was asked for while the phase was the innermost one.
  std::uint64_t reads{0};
  std::uint64_t read_bytes{0};
  std::uint64_t read_wait_ns{0};
};

// The CPU time of a thread over its life.
struct ThreadStat {
  ThreadRole role{ThreadRole::Other};
  std::uint64_t user_ns{0};
  std::uint64_t kernel_ns{0};
  // From the session start to the end of the thread, for the main thread from
  // Start() to Stop().
  std::uint64_t wall_ns{0};
};

// The reads of one ReadKind, as the volume was asked for them.
struct KindStat {
  ReadKind kind{ReadKind::Other};
  std::uint64_t reads{0};
  std::uint64_t bytes{0};
  std::uint64_t wait_ns{0};
  Histogram sizes;
  // Reads by ReadClass.
  std::array<std::uint64_t, read_class_count> by_class{};
};

// What the disk was asked for, as Win32DiskReader issued it: the synchronous
// reads and the reads ahead, aligned to 4096 bytes. All zero for a reader that
// does not report it.
struct DiskStat {
  std::uint64_t reads{0};
  std::uint64_t bytes{0};
  // Reads that were issued as a read ahead of the first queue of announced
  // ranges, and their bytes. The reads of the standing lane are not in them.
  std::uint64_t prefetch_reads{0};
  std::uint64_t prefetch_bytes{0};
  // Bytes of reads ahead that no read used before they were dropped.
  std::uint64_t wasted_bytes{0};
  // The same three for the reads issued as a read ahead of the standing lane
  // (IDiskReader::PrefetchStanding()). A read of the disk is counted in
  // reads and bytes, and in exactly one of the synchronous reads, prefetch_*
  // and standing_*.
  std::uint64_t standing_reads{0};
  std::uint64_t standing_bytes{0};
  std::uint64_t standing_wasted_bytes{0};
  // Reads that did not start where the previous one issued ended, and the sum
  // of the distances.
  std::uint64_t seeks{0};
  std::uint64_t seek_bytes{0};
  Histogram sizes;
  // Reads in flight when a read was issued, itself included; the value is the
  // index (0 to 64, 64 meaning 64 or more).
  std::array<std::uint64_t, 65> queue_depth{};
};

// The summary of a session.
struct Report {
  // Wall clock at Start(), in nanoseconds since the Unix epoch, to align the
  // report with samples taken from outside.
  std::uint64_t start_unix_ns{0};
  // From Start() to Stop().
  std::uint64_t wall_ns{0};
  // End of the last read of the volume, and the time after it: the serial tail
  // that no read can hide.
  std::uint64_t last_read_end_ns{0};
  std::uint64_t tail_ns{0};
  std::uint64_t volume_bytes{0};
  // Calls of ReadInto() on the volume and their bytes: the bytes the caller
  // asked for.
  std::uint64_t requested_reads{0};
  std::uint64_t requested_bytes{0};
  // Time the callers spent inside ReadInto(), and inside it waiting.
  std::uint64_t read_call_ns{0};
  std::uint64_t read_wait_ns{0};
  std::uint64_t prefetch_calls{0};
  std::uint64_t prefetch_bytes{0};
  std::uint64_t cancel_calls{0};
  // The calls of the standing lane that reached the reader: announced ranges
  // (and their bytes) and withdrawals.
  std::uint64_t standing_prefetch_calls{0};
  std::uint64_t standing_prefetch_bytes{0};
  std::uint64_t standing_cancel_calls{0};
  // Events kept in the log, and those dropped past the cap.
  std::uint64_t events_logged{0};
  std::uint64_t events_dropped{0};
  // CPU time of the whole process, and of the main thread.
  std::uint64_t process_user_ns{0};
  std::uint64_t process_kernel_ns{0};
  std::vector<PhaseStat> phases;
  std::vector<ThreadStat> threads;
  // Time of the main thread in each Slice.
  std::array<std::uint64_t, slice_count> slices_ns{};
  std::array<KindStat, read_kind_count> kinds{};
  // Reads by ReadClass over all kinds, and the time the callers waited in each.
  std::array<std::uint64_t, read_class_count> class_reads{};
  std::array<std::uint64_t, read_class_count> class_wait_ns{};
  DiskStat disk;
};

// The event types of the read log.
enum class EventType : std::uint8_t {
  // A ReadInto() that reached the reader.
  Read = 'R',
  // A Prefetch().
  Prefetch = 'P',
  // A CancelPrefetch().
  Cancel = 'C',
  // A SetReadPattern(): length holds the Tuning::Pattern.
  Pattern = 'T',
  // A phase began ('B') or ended ('E'): length holds the Phase.
  PhaseBegin = 'B',
  PhaseEnd = 'E',
  // A PrefetchStanding(): a range announced on the standing lane. Version 2.
  StandingPrefetch = 'S',
  // A CancelStanding(): the interval withdrawn from the standing lane, where
  // offset 0 and length UINT64_MAX is the whole lane. Version 2.
  StandingCancel = 'X'
};

// One entry of the read log. The file holds them as they are, 48 bytes each,
// little endian, after a 64-byte header (see ReadLogHeader in the sources).
struct LogEvent {
  // Start and end of the call, in nanoseconds from the session start. A call
  // that takes no time, or a marker, has both equal.
  std::uint64_t start_ns{0};
  std::uint64_t end_ns{0};
  // Time the caller spent waiting for the disk inside the call.
  std::uint64_t wait_ns{0};
  std::uint64_t offset{0};
  std::uint64_t length{0};
  EventType type{EventType::Read};
  ReadKind kind{ReadKind::Other};
  ReadClass cls{ReadClass::Unclassified};
  // The Phase that was innermost when the call began.
  std::uint8_t phase{0};
  // Reads in flight when the call began, itself included, when known.
  std::uint32_t queue_depth{0};
};

static_assert(sizeof(LogEvent) == 48);

// The read log of a session, to write or to replay.
struct ReadLog {
  std::uint64_t volume_bytes{0};
  std::uint64_t start_unix_ns{0};
  std::vector<LogEvent> events;
};

// Where a session captures what the run reads (--capture). The library calls
// Write() with every range a ReadInto() of the volume returned, in order.
class ICaptureSink {
 public:
  ICaptureSink() = default;
  ICaptureSink(const ICaptureSink&) = delete;
  ICaptureSink& operator=(const ICaptureSink&) = delete;
  ICaptureSink(ICaptureSink&&) = delete;
  ICaptureSink& operator=(ICaptureSink&&) = delete;
  virtual ~ICaptureSink() = default;

  // The bytes that were read at offset of the volume. A failure is the sink's
  // to report.
  virtual void Write(std::uint64_t offset, std::span<const BYTE> bytes) = 0;
};

struct SessionOptions {
  // Events the read log keeps at most. Past it, events are counted and
  // dropped, and the report says so. 2^25 events are 1.5 GiB.
  std::size_t max_events{std::size_t{1} << 25U};
  // False keeps the statistics but no log.
  bool keep_log{true};
};

// Begins a session: from now on the volume's reads, hints and phases are
// timed. A running session restarts.
NTFS_BROWSER_EXPORT void Start(const SessionOptions& options = {});

// Whether a session runs.
[[nodiscard]] NTFS_BROWSER_EXPORT bool Enabled() noexcept;

// Ends the session and returns what it measured. The log stays for
// TakeReadLog().
[[nodiscard]] NTFS_BROWSER_EXPORT Report Stop();

// Hands out the log of the last session, and empties it.
[[nodiscard]] NTFS_BROWSER_EXPORT ReadLog TakeReadLog();

// Writes log to path in the binary format of the read log. False, with the
// reason in error, when the file cannot be written.
[[nodiscard]] NTFS_BROWSER_EXPORT bool
    WriteReadLog(const std::filesystem::path& path, const ReadLog& log,
                 std::string* error);

// Reads a file WriteReadLog() wrote, in version 2 or in version 1 (which has
// no standing lane events). Nothing, with the reason in error, when the file is
// missing, cut or of another version.
[[nodiscard]] NTFS_BROWSER_EXPORT std::optional<ReadLog>
    LoadReadLog(const std::filesystem::path& path, std::string* error);

// Makes the library stop after phase: the scan skips the phases that follow
// and the tools skip their own. The result of a scan stopped this way is not a
// complete tree. Nothing removes the request.
NTFS_BROWSER_EXPORT void SetStopAfter(std::optional<Phase> phase);

// Whether the phase set by SetStopAfter() has ended.
[[nodiscard]] NTFS_BROWSER_EXPORT bool StopRequested() noexcept;

// Captures what the volume reads into sink, whether a session runs or not.
// Null removes it. The sink MUST outlive the volume.
NTFS_BROWSER_EXPORT void SetCaptureSink(std::shared_ptr<ICaptureSink> sink);

// Times a phase. A no-op outside a session.
class NTFS_BROWSER_EXPORT PhaseScope {
 public:
  explicit PhaseScope(Phase phase) noexcept;
  PhaseScope(const PhaseScope&) = delete;
  PhaseScope& operator=(const PhaseScope&) = delete;
  PhaseScope(PhaseScope&&) = delete;
  PhaseScope& operator=(PhaseScope&&) = delete;
  ~PhaseScope();

 private:
  Phase phase_;
  bool active_;
  std::uint64_t begin_ns_{0};
  std::uint64_t cpu_begin_ns_{0};
};

// Reports the CPU time of the calling thread when it goes out of scope. Put one
// at the top of the body of a worker thread. A no-op outside a session.
class NTFS_BROWSER_EXPORT ThreadScope {
 public:
  explicit ThreadScope(ThreadRole role) noexcept;
  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;
  ThreadScope(ThreadScope&&) = delete;
  ThreadScope& operator=(ThreadScope&&) = delete;
  ~ThreadScope();

 private:
  ThreadRole role_;
  bool active_;
  std::uint64_t begin_ns_{0};
};

// How Replay() issues the announcements.
enum class ReplayMode : std::uint8_t {
  // The Prefetch(), CancelPrefetch(), PrefetchStanding(), CancelStanding()
  // and SetReadPattern() calls of the log, in their place among the reads: the
  // run as it was, with no parsing.
  AsRecorded,
  // The announcements of the log, the standing lane's included, are dropped,
  // and every read is announced ahead of its turn instead, as far as
  // announce_ahead allows: the floor of a plan that knows every read in
  // advance.
  AllAnnounced
};

struct ReplayOptions {
  ReplayMode mode{ReplayMode::AsRecorded};
  // AllAnnounced: reads kept announced ahead of the one being made. It MUST
  // stay below the reader's cap on announced ranges, or the reader drops hints.
  std::size_t announce_ahead{512};
};

struct ReplayResult {
  // From the first event to the last, on the steady clock.
  std::uint64_t wall_ns{0};
  std::uint64_t reads{0};
  std::uint64_t read_bytes{0};
  // Reads that failed, or that were too long to replay.
  std::uint64_t failed_reads{0};
  std::uint64_t prefetches{0};
  std::uint64_t cancels{0};
  // The standing lane calls made: ranges announced, and withdrawals.
  std::uint64_t standing_prefetches{0};
  std::uint64_t standing_cancels{0};
};

// Replays the reads of log through reader, in order, with nothing between
// them: no parsing, no output. The time it takes is what the disk costs of the
// run the log was taken from. When a session runs, the replayed reads are
// timed and classified like the real ones. reader MUST be open.
[[nodiscard]] NTFS_BROWSER_EXPORT ReplayResult
    Replay(const IDiskReader& reader, const ReadLog& log,
           const ReplayOptions& options = {});

// The reader that NtfsVolume opens for a path on this platform, with the given
// read-ahead caps: a Win32DiskReader on Windows, null elsewhere, where the
// tools bring their own. It lets a replay run through the production reader.
[[nodiscard]] NTFS_BROWSER_EXPORT std::unique_ptr<IDiskReader>
    OpenPlatformReader(
        std::wstring_view path,
        const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& tuning);

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
