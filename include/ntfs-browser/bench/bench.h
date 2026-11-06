#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/bench/platform-reader.h>
#include <ntfs-browser/bench/read-log.h>
#include <ntfs-browser/bench/replay.h>
#include <ntfs-browser/bench/types.h>
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
  // The kind of device the volume planned its reads for: the option of the
  // caller when it set one, else what the reader said. Unknown when no
  // volume opened during the session.
  Tuning::MediaKind media_kind{Tuning::MediaKind::Unknown};
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

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
