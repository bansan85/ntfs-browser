#include "comparison-table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>

#include "arguments.h"
#include "bench/bench.h"
#include "engine.h"

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// Nanoseconds in a millisecond, for the time columns.
constexpr double ns_per_ms = 1e6;

}  // namespace

EngineRow MakeRow(Engine engine, const Outcome& outcome) {
  EngineRow row;
  row.engine = engine;
  row.exit_code = outcome.exit_code;
  row.entries = outcome.entries;
  row.scan = outcome.scan;
  if (!outcome.report) {
    return row;
  }
  const Bench::Report& report = *outcome.report;
  row.wall_ns = report.wall_ns;
  row.cpu_ns = report.process_user_ns + report.process_kernel_ns;
  const bool disk_counts = report.disk.reads != 0;
  row.reads = disk_counts ? report.disk.reads : report.requested_reads;
  row.bytes = disk_counts ? report.disk.bytes : report.requested_bytes;
  return row;
}

std::string FormatTable(std::span<const EngineRow> rows) {
  constexpr size_t line_size = 160;
  std::string table;
  std::array<char, line_size> line{};
  auto add = [&](int length) {
    if (length > 0) {
      table.append(line.data(),
                   (std::min)(static_cast<size_t>(length), line.size() - 1));
    }
  };
  add(snprintf(line.data(), line.size(), "%-10s %5s %12s %12s %12s %10s %14s\n",
               "Engine", "Exit", "Wall (ms)", "CPU (ms)", "Entries", "Reads",
               "Bytes"));
  for (const EngineRow& row : rows) {
    add(snprintf(line.data(), line.size(),
                 "%-10s %5d %12.1f %12.1f %12llu %10llu %14llu\n",
                 std::string(EngineName(row.engine)).c_str(), row.exit_code,
                 static_cast<double>(row.wall_ns) / ns_per_ms,
                 static_cast<double>(row.cpu_ns) / ns_per_ms,
                 static_cast<unsigned long long>(row.entries),
                 static_cast<unsigned long long>(row.reads),
                 static_cast<unsigned long long>(row.bytes)));
  }
  return table;
}

std::string FormatScanTables(std::span<const EngineRow> rows) {
  constexpr size_t line_size = 200;
  std::string tables;
  std::array<char, line_size> line{};
  auto add = [&](int length) {
    if (length > 0) {
      tables.append(line.data(),
                    (std::min)(static_cast<size_t>(length), line.size() - 1));
    }
  };
  const auto ms = [](std::uint64_t ns) {
    return static_cast<double>(ns) / ns_per_ms;
  };
  const auto name = [](const EngineRow& row) {
    return std::string(EngineName(row.engine));
  };
  bool any = false;
  for (const EngineRow& row : rows) {
    any = any || row.scan;
  }
  if (!any) {
    return tables;
  }

  tables += "\nVerdicts (T_io: read log replayed. T_cpu: a second, warm run)\n";
  add(snprintf(line.data(), line.size(), "%-10s %10s %10s %10s %8s %8s %-10s\n",
               "Engine", "Real (ms)", "IO (ms)", "CPU (ms)", "IO/Real", "R",
               "Path"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const ScanFigures& figures = row.scan->figures;
    const Verdict& verdict = row.scan->verdict;
    add(snprintf(line.data(), line.size(),
                 "%-10s %10.1f %10.1f %10.1f %8.2f %8.2f %-10s\n",
                 name(row).c_str(), ms(figures.wall_ns), ms(figures.io_ns),
                 ms(figures.cpu_ns), verdict.io_share,
                 verdict.overlap.value_or(0.0),
                 std::string(CriticalPathName(verdict.path)).c_str()));
  }

  tables += "\nWorker thread of the scan (ms)\n";
  add(snprintf(line.data(), line.size(),
               "%-10s %10s %10s %10s %10s %10s %10s %12s %12s\n", "Engine",
               "Wall", "CPU", "Wait", "Match", "Exchange", "Caller CPU", "Seen",
               "Delivered"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const WorkerFigures& worker = row.scan->measure.worker;
    add(snprintf(
        line.data(), line.size(),
        "%-10s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %12llu %12llu\n",
        name(row).c_str(), ms(worker.wall_ns), ms(worker.cpu_ns),
        ms(worker.wait_ns), ms(worker.match_ns), ms(worker.exchange_ns),
        ms(row.scan->figures.caller_cpu_ns),
        static_cast<unsigned long long>(worker.entries_seen),
        static_cast<unsigned long long>(worker.entries_delivered)));
  }

  tables += "\nParallelism\n";
  add(snprintf(line.data(), line.size(), "%-10s %12s %8s %8s %10s %12s %12s\n",
               "Engine", "CPU (ms)", "Threads", "Cores", "Efficiency",
               "Worker/Real", "Worker/CPU"));
  for (const EngineRow& row : rows) {
    if (!row.scan) {
      continue;
    }
    const ScanFigures& figures = row.scan->figures;
    const Verdict& verdict = row.scan->verdict;
    add(snprintf(line.data(), line.size(),
                 "%-10s %12.1f %8u %8.2f %10.2f %12.2f %12.2f\n",
                 name(row).c_str(), ms(figures.total_cpu_ns), figures.threads,
                 verdict.cores_busy, verdict.parallel_efficiency,
                 verdict.worker_cpu_share, verdict.worker_cpu_of_total));
  }
  return tables;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
