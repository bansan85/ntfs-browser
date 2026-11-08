#include "search/scan-probe.h"

#include <utility>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// The timings that the next scan fills. Null in every real use.
ScanTimings* g_probe_target = nullptr;

// The factory of SetReaderFactoryForTests(). Empty in every real use.
ReaderFactory& ReaderFactoryStorage() {
  static ReaderFactory factory;
  return factory;
}

// The accumulators of the calling thread, set on the worker of a probed scan.
thread_local ScanTimings* t_worker_timings = nullptr;

}  // namespace

void SetScanProbeForTests(ScanTimings* timings) { g_probe_target = timings; }

ScanTimings* ScanProbeTarget() noexcept { return g_probe_target; }

void SetReaderFactoryForTests(ReaderFactory factory) {
  ReaderFactoryStorage() = std::move(factory);
}

const ReaderFactory& ReaderFactoryOverride() noexcept {
  return ReaderFactoryStorage();
}

ScanTimings* WorkerTimings() noexcept { return t_worker_timings; }

WorkerTimingsScope::WorkerTimingsScope(ScanTimings* timings) noexcept {
  t_worker_timings = timings;
}

WorkerTimingsScope::~WorkerTimingsScope() { t_worker_timings = nullptr; }

}  // namespace NTFS_BROWSER_NAMESPACE::Search
