#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

#include "bench/bench.h"
#include "bench/platform-reader.h"
#include "bench/replay.h"
#include "disk-reader.h"
#include "search/scan-probe.h"
#include "search/target.h"
#include "tuning/tuning.h"

#ifndef _WIN32
  #include <ntfs-browser/tool/device-access/posix-device-disk-reader.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// How often Run() polls: the interval of the console tools.
constexpr std::chrono::milliseconds poll_interval{100};

}  // namespace

ScanMeasure MeasureScan(const NTFS_BROWSER_NAMESPACE::Options& options) {
  NTFS_BROWSER_NAMESPACE::ScanTimings timings;
  NTFS_BROWSER_NAMESPACE::SetScanProbeForTests(&timings);
  ScanMeasure measure;
  {
    NTFS_BROWSER_NAMESPACE::Scan scan(options);
    scan.OnBatch([&](const NTFS_BROWSER_NAMESPACE::Batch& batch) {
      measure.entries += batch.entries.size();
    });
    const auto begin = std::chrono::steady_clock::now();
    measure.state = scan.Run(poll_interval);
    measure.wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
  }
  NTFS_BROWSER_NAMESPACE::SetScanProbeForTests(nullptr);
  measure.worker = {.wall_ns = timings.worker_wall_ns,
                    .cpu_ns = timings.worker_cpu_ns,
                    .wait_ns = timings.wait_ns,
                    .match_ns = timings.match_ns,
                    .exchange_ns = timings.exchange_ns,
                    .entries_seen = timings.entries_seen,
                    .entries_delivered = timings.entries_delivered};
  return measure;
}

std::optional<std::uint64_t> ReplayNs(
    std::wstring_view target, const Bench::ReadLog& log,
    const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& caps) {
  const std::optional<NTFS_BROWSER_NAMESPACE::TargetParts> parts =
      NTFS_BROWSER_NAMESPACE::SplitTarget(target);
  if (!parts) {
    return std::nullopt;
  }
  std::wstring path = parts->source;
  if (parts->kind == NTFS_BROWSER_NAMESPACE::TargetParts::Kind::Drive) {
    path = std::wstring(L"\\.\\") + parts->source + L":";
  }
  std::unique_ptr<IDiskReader> reader;
#ifdef _WIN32
  reader = Bench::OpenPlatformReader(path, caps);
#else
  static_cast<void>(caps);
  auto posix = std::make_unique<Tool::DeviceAccess::PosixDeviceDiskReader>();
  if (posix->Open(path)) {
    reader = std::move(posix);
  }
#endif
  if (!reader) {
    return std::nullopt;
  }
  return Bench::Replay(*reader, log).wall_ns;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
