#pragma once

// The measure of a Search::Scan, run as the library runs it: one worker thread
// that fills the exchange, and a caller that polls. It answers two questions:
// whether the disk or the CPU bounds the run, and whether the CPU work runs in
// parallel. The scan runs under the test seams of src/search/scan-probe.h, so
// that the worker reports where its time went.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "bench/bench.h"
#include "bench/read-log.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// What the worker thread of a scan spent its time on, in nanoseconds. The
// fields are those of the internal Search::ScanTimings.
struct WorkerFigures {
  std::uint64_t wall_ns{0};
  std::uint64_t cpu_ns{0};
  // Waiting for the caller to take the pending entries.
  std::uint64_t wait_ns{0};
  // The pattern and the filters.
  std::uint64_t match_ns{0};
  // The lock and the copy of each entry into the exchange.
  std::uint64_t exchange_ns{0};
  // Entries the engine offered, and entries that passed the match stage.
  std::uint64_t entries_seen{0};
  std::uint64_t entries_delivered{0};
};

// One scan, run to its end.
struct ScanMeasure {
  NTFS_BROWSER_NAMESPACE::Search::State state{
      NTFS_BROWSER_NAMESPACE::Search::State::NotStarted};
  // Entries the callback received.
  std::uint64_t entries{0};
  // From the start of the scan to the end of Run(), on the steady clock.
  std::uint64_t wall_ns{0};
  WorkerFigures worker;
};

// Runs a Search::Scan with options to its end on the calling thread, polling
// every 100 ms, with the worker timed. The scan reads through whatever reader
// the tests set with SetReaderFactoryForTests(), else through the file or
// device of options.target. The probe is removed when the call returns.
[[nodiscard]] ScanMeasure
    MeasureScan(const NTFS_BROWSER_NAMESPACE::Search::Options& options);

// Everything that the verdicts of a run are made of, in nanoseconds.
struct ScanFigures {
  // T_real: the run, from the start of the session to its end.
  std::uint64_t wall_ns{0};
  // T_io: what the disk costs of the run, alone: the read log replayed with
  // nothing between the reads. 0 when not measured.
  std::uint64_t io_ns{0};
  // T_cpu: the run again with every read served from memory, so that only the
  // CPU is left. 0 when not measured.
  std::uint64_t cpu_ns{0};
  // CPU time of the process, user and kernel, all threads.
  std::uint64_t total_cpu_ns{0};
  // CPU time of the caller thread, and of the worker thread of the scan.
  std::uint64_t caller_cpu_ns{0};
  std::uint64_t worker_cpu_ns{0};
  // Threads used: those that reported (the caller, the worker, the threads of
  // the engine), or the cores kept busy on average when that is more, since a
  // thread of the read-ahead reader does not report. At least 1.
  unsigned threads{1};
};

// Which resource bounds the run, by the rule of "ntfs_benchmark.py audit".
enum class CriticalPath : std::uint8_t {
  // T_io or T_cpu is missing.
  Unknown,
  // The CPU hides under the I/O: T_real is within 5% of T_io.
  IoStrong,
  // T_real is within 10% of T_io, and T_cpu is below T_io.
  Io,
  // T_real is within 10% of T_cpu, and T_cpu is not below T_io.
  Cpu,
  // Neither floor explains the time.
  Mixed
};

// The derived figures of a run.
struct Verdict {
  CriticalPath path{CriticalPath::Unknown};
  // T_io / T_real: 1 when the disk accounts for all the time. 0 when unknown.
  double io_share{0.0};
  // R = (T_io + T_cpu - T_real) / min(T_io, T_cpu): 1 when the CPU work hides
  // wholly under the I/O, 0 when the two run one after the other.
  std::optional<double> overlap;
  // Total CPU / T_real: how many cores the run kept busy on average.
  double cores_busy{0.0};
  // Total CPU / (T_real x threads): 1 when every thread computes all the
  // time. The caller thread sleeps, so it counts as a thread that idles.
  double parallel_efficiency{0.0};
  // CPU of the worker thread / T_real: the share of the run that the one
  // thread of the scan spends computing.
  double worker_cpu_share{0.0};
  // CPU of the worker thread / total CPU: how much of the CPU work is serial.
  double worker_cpu_of_total{0.0};
};

// The verdicts of figures. Every ratio is 0 when its divisor is.
[[nodiscard]] Verdict Judge(const ScanFigures& figures);

// The name of path in the tables and the JSON: "unknown", "io-strong", "io",
// "cpu" or "mixed".
[[nodiscard]] std::string_view CriticalPathName(CriticalPath path) noexcept;

// The figures of a run that had a bench session: wall, CPU of the process, of
// the caller (the main thread of the session) and the thread count come from
// report, the worker CPU from measure. io_ns and cpu_ns are left to the caller.
[[nodiscard]] ScanFigures
    FiguresOf(const NTFS_BROWSER_NAMESPACE::Bench::Report& report,
              const ScanMeasure& measure);

// T_io: the time the reads of log take on target, with nothing between them.
// target is an image file, or a drive letter on Windows. Nothing when the
// target cannot be opened. caps are the read-ahead caps of the run.
[[nodiscard]] std::optional<std::uint64_t> ReplayNs(
    std::wstring_view target, const NTFS_BROWSER_NAMESPACE::Bench::ReadLog& log,
    const std::array<NTFS_BROWSER_NAMESPACE::Tuning::ReaderTuning,
                     NTFS_BROWSER_NAMESPACE::Tuning::pattern_count>& caps);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
