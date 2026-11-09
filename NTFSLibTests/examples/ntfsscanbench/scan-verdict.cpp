#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>

#include "bench/bench.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// How far above a floor T_real may be and still be said to sit on it. 5% is a
// strong match and 10% a plain one, as in ntfs_benchmark.py.
constexpr double critical_strong = 1.05;
constexpr double critical_plain = 1.10;

// numerator / denominator, or 0 when the denominator is 0.
double Ratio(std::uint64_t numerator, double denominator) {
  return denominator > 0.0 ? static_cast<double>(numerator) / denominator : 0.0;
}

// The path of a run from T_real, T_io and T_cpu, all known and above 0.
CriticalPath PathOf(const ScanFigures& figures) {
  const auto real = static_cast<double>(figures.wall_ns);
  const bool io_bound =
      figures.cpu_ns < figures.io_ns &&
      real <= critical_plain * static_cast<double>(figures.io_ns);
  if (io_bound) {
    return real <= critical_strong * static_cast<double>(figures.io_ns)
               ? CriticalPath::IoStrong
               : CriticalPath::Io;
  }
  if (figures.cpu_ns >= figures.io_ns &&
      real <= critical_plain * static_cast<double>(figures.cpu_ns)) {
    return CriticalPath::Cpu;
  }
  return CriticalPath::Mixed;
}

}  // namespace

Verdict Judge(const ScanFigures& figures) {
  Verdict verdict;
  const auto real = static_cast<double>(figures.wall_ns);
  verdict.io_share = Ratio(figures.io_ns, real);
  verdict.cores_busy = Ratio(figures.total_cpu_ns, real);
  verdict.parallel_efficiency =
      Ratio(figures.total_cpu_ns, real * std::max(1U, figures.threads));
  verdict.worker_cpu_share = Ratio(figures.worker_cpu_ns, real);
  verdict.worker_cpu_of_total =
      Ratio(figures.worker_cpu_ns, static_cast<double>(figures.total_cpu_ns));
  if (figures.wall_ns == 0 || figures.io_ns == 0 || figures.cpu_ns == 0) {
    return verdict;
  }
  const auto smaller =
      static_cast<double>((std::min)(figures.io_ns, figures.cpu_ns));
  verdict.overlap = (static_cast<double>(figures.io_ns) +
                     static_cast<double>(figures.cpu_ns) - real) /
                    smaller;
  verdict.path = PathOf(figures);
  return verdict;
}

std::string_view CriticalPathName(CriticalPath path) noexcept {
  switch (path) {
    case CriticalPath::Unknown:
      break;
    case CriticalPath::IoStrong:
      return "io-strong";
    case CriticalPath::Io:
      return "io";
    case CriticalPath::Cpu:
      return "cpu";
    case CriticalPath::Mixed:
      return "mixed";
  }
  return "unknown";
}

ScanFigures FiguresOf(const Bench::Report& report, const ScanMeasure& measure) {
  ScanFigures figures;
  figures.wall_ns = report.wall_ns;
  figures.total_cpu_ns = report.process_user_ns + report.process_kernel_ns;
  figures.worker_cpu_ns = measure.worker.cpu_ns;
  for (const Bench::ThreadStat& thread : report.threads) {
    if (thread.role == Bench::ThreadRole::Main) {
      figures.caller_cpu_ns = thread.user_ns + thread.kernel_ns;
    }
  }
  // Threads that do not report (the read-ahead threads of a volume) still burn
  // CPU: the cores kept busy on average are a floor for the threads used.
  const auto cores = static_cast<unsigned>(
      report.wall_ns == 0
          ? 0.0
          : std::ceil(static_cast<double>(figures.total_cpu_ns) /
                      static_cast<double>(report.wall_ns)));
  figures.threads =
      (std::max)({1U, static_cast<unsigned>(report.threads.size()), cores});
  return figures;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
