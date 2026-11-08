#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/bench/platform-reader.h>
#include <ntfs-browser/bench/replay.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tuning/tuning.h>

#include "search/scan-probe.h"
#include "search/target.h"

#ifndef _WIN32
  #include <ntfs-browser/tool/device-access/posix-device-disk-reader.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// How often Run() polls: the interval of the console tools.
constexpr std::chrono::milliseconds poll_interval{100};

}  // namespace

ScanMeasure MeasureScan(const Search::Options& options) {
  Search::ScanTimings timings;
  Search::SetScanProbeForTests(&timings);
  ScanMeasure measure;
  {
    Search::Scan scan(options);
    scan.OnBatch([&](const Search::Batch& batch) {
      measure.entries += batch.entries.size();
    });
    const auto begin = std::chrono::steady_clock::now();
    measure.state = scan.Run(poll_interval);
    measure.wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
  }
  Search::SetScanProbeForTests(nullptr);
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
  const std::optional<Search::TargetParts> parts = Search::SplitTarget(target);
  if (!parts) {
    return std::nullopt;
  }
  std::wstring path = parts->source;
  if (parts->kind == Search::TargetParts::Kind::Drive) {
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
