#pragma once

// The test seams of Scan. A benchmark or a test sets them while no scan
// runs. Without them a scan reads, hints, logs and outputs exactly what it did
// before: the worker only pays one test of a null pointer for each entry.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include <ntfs-browser/namespace.h>

#include "disk-reader.h"
#include "internal-export.h"
#include "search/target.h"

namespace NTFS_BROWSER_NAMESPACE {

// What the worker thread of one scan spent its time on. All times are
// nanoseconds. The engine's own time (reading and parsing) is what remains of
// worker_wall_ns when the other four are taken out.
struct ScanTimings {
  // From the start of the worker to its end, on the steady clock.
  std::uint64_t worker_wall_ns{0};
  // CPU time of the worker thread (user and kernel).
  std::uint64_t worker_cpu_ns{0};
  // Time Add() waited because the pending buffer held max_pending_entries.
  std::uint64_t wait_ns{0};
  // Time the pattern and the filters took (MatchingSink).
  std::uint64_t match_ns{0};
  // Time Add() took without the wait: the lock and the copy of the entry.
  std::uint64_t exchange_ns{0};
  // Entries the engine gave to the MatchingSink, and those that reached the
  // exchange.
  std::uint64_t entries_seen{0};
  std::uint64_t entries_delivered{0};
};

// Makes every scan that starts from now on fill timings when its worker ends.
// Null removes it. timings MUST outlive the scans. Not thread safe: call it
// while no scan runs. The numbers are ready when Run() has returned or when
// Poll() has delivered the last batch.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetScanProbeForTests(ScanTimings* timings);

// The timings that the next scan is to fill, or null.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ScanTimings*
    ScanProbeTarget() noexcept;

// A function that opens the reader of a target in place of the file or device.
// It returns an open reader, or null when it cannot.
using ReaderFactory =
    std::function<std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>(
        const TargetParts& parts)>;

// Makes OpenVolume() read through the reader that factory returns, such as a
// simulated disk. An empty function restores the real readers. Not thread safe:
// call it while no scan runs.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetReaderFactoryForTests(ReaderFactory factory);

// The factory set above, or an empty function.
[[nodiscard]] const ReaderFactory& ReaderFactoryOverride() noexcept;

// The accumulators of the worker thread of a scan that runs under a probe, or
// null on any other thread and when no probe is set. The worker calls it once
// for each entry.
[[nodiscard]] ScanTimings* WorkerTimings() noexcept;

// Makes timings the accumulators of the calling thread until it goes out of
// scope. Null does nothing.
class WorkerTimingsScope {
 public:
  explicit WorkerTimingsScope(ScanTimings* timings) noexcept;
  WorkerTimingsScope(const WorkerTimingsScope&) = delete;
  WorkerTimingsScope& operator=(const WorkerTimingsScope&) = delete;
  WorkerTimingsScope(WorkerTimingsScope&&) = delete;
  WorkerTimingsScope& operator=(WorkerTimingsScope&&) = delete;
  ~WorkerTimingsScope();
};

// Adds the time from its construction to its destruction to a counter of the
// timings of the calling thread. Does nothing, and reads no clock, when the
// thread has none.
class StageTimer {
 public:
  explicit StageTimer(std::uint64_t ScanTimings::* counter) noexcept
      : timings_(WorkerTimings()), counter_(counter) {
    if (timings_ != nullptr) {
      begin_ = std::chrono::steady_clock::now();
    }
  }

  StageTimer(const StageTimer&) = delete;
  StageTimer& operator=(const StageTimer&) = delete;
  StageTimer(StageTimer&&) = delete;
  StageTimer& operator=(StageTimer&&) = delete;

  ~StageTimer() {
    if (timings_ != nullptr) {
      timings_->*counter_ += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - begin_)
              .count());
    }
  }

 private:
  ScanTimings* timings_;
  std::uint64_t ScanTimings::* counter_;
  std::chrono::steady_clock::time_point begin_;
};

}  // namespace NTFS_BROWSER_NAMESPACE
